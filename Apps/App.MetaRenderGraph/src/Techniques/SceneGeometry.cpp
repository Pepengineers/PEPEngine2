#include "SceneGeometry.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include "Engine.RendererDX12/GDX12ShaderCompiler.h"
#include "Engine.RendererDX12/GDX12TextureResource.h"
#include <algorithm>
#include <fstream>

namespace Meta {
namespace {
constexpr DXGI_FORMAT Formats[]={DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R16G16_FLOAT,
    DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R16G16B16A16_FLOAT};
constexpr State ShaderRead=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
}
SceneGeometry::SceneGeometry(GDX12DeviceResources& r):_resources(r),_slots(r.FrameConstants.size()) {
    GDX12RootSignatureDesc d;
    d.NumSingleCBVSlots=1; d.NumSingleSRVSlots=3; d.NumSingleUAVSlots=4;
    _cullRoot=std::make_unique<GDX12RootSignature>(r.Device,d);
    _cull=GDX12RenderHelpers::CreateComputePipeline(r.Device,_cullRoot.get(),SHADERS_FOLDER "Culling.hlsl","CS");
    d={}; d.NumSingleUAVSlots=2;
    _clearRoot=std::make_unique<GDX12RootSignature>(r.Device,d);
    _clear=GDX12RenderHelpers::CreateComputePipeline(r.Device,_clearRoot.get(),SHADERS_FOLDER "BufferClear.hlsl","CS");
    d={}; d.InstancedGeometry=true; d.Constants={1}; d.NumSingleCBVSlots=2;
    d.NumSingleSRVSlots=3; d.SRVRanges.emplace_back(Texture2D_RangeLength); d.StaticSamplers=GetStaticSamplers();
    _drawRoot=std::make_unique<GDX12RootSignature>(r.Device,d);
    _signature=GDX12RenderHelpers::CreateIndexedDrawSignature(r.Device,_drawRoot.get());
    auto& compiler=GDX12ShaderCompiler::GetInstance();
    D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};
    p.pRootSignature=_drawRoot->GetRootSignature().Get();
    p.InputLayout={r.InputLayouts.at("Default").data(),UINT(r.InputLayouts.at("Default").size())};
    p.RasterizerState=CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT); p.RasterizerState.FrontCounterClockwise=TRUE;
    p.BlendState=CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    p.DepthStencilState=CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    p.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    p.DSVFormat=DXGI_FORMAT_D32_FLOAT; p.SampleMask=UINT_MAX; p.SampleDesc.Count=1;
    p.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    auto vs=compiler.CompileShader(r.Device,SHADERS_FOLDER "OpaquePass.hlsl",nullptr,"DepthVS","vs");
    auto ps=compiler.CompileShader(r.Device,SHADERS_FOLDER "OpaquePass.hlsl",nullptr,"DepthPS","ps");
    p.VS={vs->GetBufferPointer(),vs->GetBufferSize()}; p.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
    ThrowIfFailed(r.Device->GetDevice()->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&_depth)));
    vs=compiler.CompileShader(r.Device,SHADERS_FOLDER "OpaquePass.hlsl",nullptr,"VS","vs");
    ps=compiler.CompileShader(r.Device,SHADERS_FOLDER "OpaquePass.hlsl",nullptr,"PS","ps");
    p.VS={vs->GetBufferPointer(),vs->GetBufferSize()}; p.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
    p.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_EQUAL; p.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ZERO;
    p.NumRenderTargets=5; std::copy(std::begin(Formats),std::end(Formats),p.RTVFormats);
    ThrowIfFailed(r.Device->GetDevice()->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&_gbuffer)));
}
void SceneGeometry::BufferFor(Buffer& b,UINT64 bytes,UINT stride,bool upload) {
    bytes=(std::max)(bytes,UINT64(stride));
    if(b.Data && b.Bytes==bytes) return;
    b.Bytes=bytes;
    if(upload) b.Source=_resources.Device->CreateResource(D3D12_HEAP_TYPE_UPLOAD,CD3DX12_RESOURCE_DESC::Buffer(bytes),D3D12_RESOURCE_STATE_GENERIC_READ);
    auto desc=CD3DX12_RESOURCE_DESC::Buffer(bytes,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    b.Data=_resources.Device->CreateResource(D3D12_HEAP_TYPE_DEFAULT,desc,D3D12_RESOURCE_STATE_COMMON);
    auto* heap=_resources.SRV_UAV_Heap.get();
    const auto srvIndex=b.Srv.HeapIndex==UINT_MAX?heap->GetAvailableIndex(ConstantsResources):b.Srv.HeapIndex;
    const auto uavIndex=b.Uav.HeapIndex==UINT_MAX?heap->GetAvailableIndex(ConstantsResources):b.Uav.HeapIndex;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{}; srv.Format=DXGI_FORMAT_UNKNOWN; srv.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; srv.Buffer.NumElements=UINT(bytes/stride); srv.Buffer.StructureByteStride=stride;
    b.Srv.InitAsSRV(b.Data.Get(),&srv,heap,srvIndex);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{}; uav.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements=UINT(bytes/stride); uav.Buffer.StructureByteStride=stride;
    b.Uav.InitAsUAV(b.Data.Get(),nullptr,&uav,heap,uavIndex);
}
void SceneGeometry::Resize(UINT width,UINT height) {
    _width=width; _height=height;
    for(auto& s:_slots) {
        for(UINT i=0;i<5;++i) {
            if(!s.Color[i]) { s.Color[i]=std::make_unique<GDX12Texture>(); GDX12RenderHelpers::InitializeRenderTarget(*s.Color[i],_resources,width,height,Formats[i],{0,0,0,0}); }
            else s.Color[i]->Resize(width,height);
        }
        auto desc=CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32_TYPELESS,width,height,1,1,1,0,D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
        D3D12_CLEAR_VALUE clear{}; clear.Format=DXGI_FORMAT_D32_FLOAT;
        s.Depth=_resources.Device->CreateResource(D3D12_HEAP_TYPE_DEFAULT,desc,D3D12_RESOURCE_STATE_COMMON,&clear);
        auto* heap=_resources.DSVHeap.get(); D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
        dsv.Format=DXGI_FORMAT_D32_FLOAT; dsv.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2D;
        s.Dsv.InitAsDSV(s.Depth.Get(),&dsv,heap,s.Dsv.HeapIndex==UINT_MAX?heap->GetAvailableIndex():s.Dsv.HeapIndex);
        dsv.Flags=D3D12_DSV_FLAG_READ_ONLY_DEPTH;
        s.ReadDepth.InitAsDSV(s.Depth.Get(),&dsv,heap,s.ReadDepth.HeapIndex==UINT_MAX?heap->GetAvailableIndex():s.ReadDepth.HeapIndex);
        UINT64 offset=0;
        for(UINT i=0;i<6;++i) {
            auto resource=i<5?s.Color[i]->GetResource()->D3DResource.Get():s.Depth.Get();
            const auto shape=resource->GetDesc(); UINT64 size=0;
            _resources.Device->GetDevice()->GetCopyableFootprints(&shape,0,1,offset,&s.Footprints[i],nullptr,nullptr,&size);
            offset=(offset+size+511)&~UINT64(511);
        }
        s.ImageBytes=offset;
        s.ImageReadback=_resources.Device->CreateResource(D3D12_HEAP_TYPE_READBACK,CD3DX12_RESOURCE_DESC::Buffer(offset),D3D12_RESOURCE_STATE_COPY_DEST);
    }
}
void SceneGeometry::Freeze(UINT index,UINT camera,std::vector<GDX12SkinningWork> skin) {
    auto& s=_slots.at(index); auto& f=*_resources.FrameConstants.at(index);
    // CPU preparation is complete. These per-frame uploads cannot change until
    // the returned frame fence; workers only record copies of assigned ranges.
    auto input=[&](UINT i,auto& buffer) {
        BufferFor(s.Buffers[i],UINT64(buffer->GetElementCount())*buffer->GetElementSize(),buffer->GetElementSize(),true);
        void* data=nullptr; D3D12_RANGE noRead{};
        ThrowIfFailed(s.Buffers[i].Source->Map(0,&noRead,&data));
        const UINT64 bytes=UINT64(buffer->GetElementCount())*buffer->GetElementSize();
        std::memset(data,0,SIZE_T(s.Buffers[i].Bytes));
        if(bytes) {
            // Material uploads have no CPU mirror in the production path.
            void* original=nullptr; const D3D12_RANGE range{0,SIZE_T(bytes)};
            ThrowIfFailed(buffer->GetResource().D3DResource->Map(0,&range,&original));
            std::memcpy(data,original,SIZE_T(bytes));
            const D3D12_RANGE noWrite{}; buffer->GetResource().D3DResource->Unmap(0,&noWrite);
        }
        s.Buffers[i].Source->Unmap(0,nullptr);
    };
    input(0,f.InstanceCache); input(1,_resources.IndirectCommandsCache); input(2,f.MaterialCache); input(3,f.TransformCache);
    s.Draws=_resources.IndirectCommandsCache->GetElementCount();
    for(UINT i:{4u,6u}) BufferFor(s.Buffers[i],UINT64((std::max)(s.Draws,1u))*sizeof(GDX12IndirectDrawArgs),sizeof(GDX12IndirectDrawArgs));
    for(UINT i:{5u,7u}) BufferFor(s.Buffers[i],4,4);
    s.Camera=f.CameraCB->GetElementAddress(camera); s.Main=f.MainCB->GetElementAddress(0);
    s.Geometry=_resources.GeometryBuffer->FreezeRecordingBinding();
    s.Skin=std::move(skin);
    s.SkinBindings.clear(); s.SkinDeclarations.clear(); s.SkinIds.clear();
    for(const auto& work:s.Skin) {
        std::array<Id,5> ids{};
        for(UINT j=0;j<5;++j) {
            const auto found=std::find(s.SkinBindings.begin(),s.SkinBindings.end(),work.Resources[j]);
            if(found!=s.SkinBindings.end()) ids[j]=23+Id(found-s.SkinBindings.begin());
            else {
                ids[j]=23+Id(s.SkinBindings.size()); s.SkinBindings.push_back(work.Resources[j]);
                const State initial=j==0?State(D3D12_RESOURCE_STATE_GENERIC_READ):0;
                s.SkinDeclarations.push_back({"skin input "+std::to_string(ids[j]),initial,initial,j==1,j!=0});
            }
        }
        s.SkinIds.push_back(ids);
    }
    UINT64 bytes=0; for(UINT i=0;i<8;++i) { s.Offsets[i]=bytes; bytes+=s.Buffers[i].Bytes; }
    s.SkinOffset=bytes; for(const auto& skin:s.Skin) bytes+=UINT64(skin.VertexCount)*sizeof(Vertex);
    if(s.SceneBytes!=bytes) {
        s.SceneBytes=bytes;
        s.SceneReadback=_resources.Device->CreateResource(D3D12_HEAP_TYPE_READBACK,CD3DX12_RESOURCE_DESC::Buffer(bytes),D3D12_RESOURCE_STATE_COPY_DEST);
    }
}
Schedule SceneGeometry::Plan(UINT index) const {
    const auto& s=_slots.at(index);
    Graph g;
    for(UINT i=0;i<8;++i) g.Resources.push_back({"scene buffer "+std::to_string(i),0,0,true,true});
    for(UINT i=0;i<4;++i) g.Resources.push_back({"frozen upload "+std::to_string(i),D3D12_RESOURCE_STATE_GENERIC_READ,D3D12_RESOURCE_STATE_GENERIC_READ});
    for(UINT i=0;i<5;++i) g.Resources.push_back({"GBuffer "+std::to_string(i),0,0});
    g.Resources.push_back({"depth",0,0}); g.Resources.push_back({"diagnostic output",0,0});
    g.Resources.push_back({"image readback",D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_DEST});
    g.Resources.push_back({"scene readback",D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_DEST});
    g.Resources.push_back({"scene vertices",0,0,true,true});
    g.Resources.push_back({"scene indices",0,0,false,true});
    g.Resources.insert(g.Resources.end(),s.SkinDeclarations.begin(),s.SkinDeclarations.end());
    Operation palettes{"skin palette uploads"}, skinning{"skin deformation"};
    const auto add=[](Operation& op,Access access) {
        if(std::none_of(op.Accesses.begin(),op.Accesses.end(),[&](const auto& a){return a.Resource==access.Resource;})) op.Accesses.push_back(access);
    };
    for(const auto& ids:s.SkinIds) {
        add(palettes,{ids[0],Mode::Read,D3D12_RESOURCE_STATE_GENERIC_READ});
        add(palettes,{ids[1],Mode::Write,D3D12_RESOURCE_STATE_COPY_DEST});
        for(UINT j=1;j<5;++j) add(skinning,{ids[j],Mode::Read,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE});
    }
    if(!s.Skin.empty()) skinning.Accesses.push_back({21,Mode::Write,UnorderedAccess});
    g.Operations.push_back(palettes); g.Operations.push_back(skinning);
    Operation upload{"scene upload buckets"}; upload.Lists=4;
    for(UINT i=0;i<4;++i) { upload.Accesses.push_back({i,Mode::Write,D3D12_RESOURCE_STATE_COPY_DEST}); upload.Accesses.push_back({8+i,Mode::Read,D3D12_RESOURCE_STATE_GENERIC_READ}); }
    g.Operations.push_back(upload);
    g.Operations.push_back({"reset visible counts",{{5,Mode::Write,UnorderedAccess},{7,Mode::Write,UnorderedAccess}}});
    Operation cull{"GPU scene culling"}; for(UINT i=0;i<3;++i) cull.Accesses.push_back({i,Mode::Read,ShaderRead});
    for(UINT i=4;i<8;++i) cull.Accesses.push_back({i,Mode::ReadWrite,UnorderedAccess});
    g.Operations.push_back(cull);
    Operation depth{"scene depth ExecuteIndirect"};
    for(UINT i:{0u,2u,3u}) depth.Accesses.push_back({i,Mode::Read,ShaderRead});
    for(UINT i:{4u,5u}) depth.Accesses.push_back({i,Mode::Read,D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT});
    depth.Accesses.push_back({17,Mode::Write,D3D12_RESOURCE_STATE_DEPTH_WRITE}); g.Operations.push_back(depth);
    auto draw=depth; draw.Name="scene GBuffer ExecuteIndirect"; draw.Accesses.back()={17,Mode::Read,D3D12_RESOURCE_STATE_DEPTH_READ};
    for(auto* operation:{&g.Operations.back(),&draw}) {
        operation->Accesses.push_back({21,Mode::Read,D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER|D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE});
        operation->Accesses.push_back({22,Mode::Read,D3D12_RESOURCE_STATE_INDEX_BUFFER});
    }
    for(UINT i=12;i<17;++i) draw.Accesses.push_back({i,Mode::Write,D3D12_RESOURCE_STATE_RENDER_TARGET}); g.Operations.push_back(draw);
    g.Operations.push_back({"diagnostic albedo copy",{{12,Mode::Read,D3D12_RESOURCE_STATE_COPY_SOURCE},{18,Mode::Write,D3D12_RESOURCE_STATE_COPY_DEST}}});
    Operation readback{"scene and image readback",{{19,Mode::Write,D3D12_RESOURCE_STATE_COPY_DEST},{20,Mode::Write,D3D12_RESOURCE_STATE_COPY_DEST}}};
    for(UINT i=0;i<8;++i) readback.Accesses.push_back({i,Mode::Read,D3D12_RESOURCE_STATE_COPY_SOURCE});
    for(UINT i=12;i<18;++i) readback.Accesses.push_back({i,Mode::Read,D3D12_RESOURCE_STATE_COPY_SOURCE});
    readback.Accesses.push_back({21,Mode::Read,D3D12_RESOURCE_STATE_COPY_SOURCE});
    g.Operations.push_back(readback);
    return Compile(g);
}
std::vector<ID3D12Resource*> SceneGeometry::Resources(UINT index,ID3D12Resource* target) const {
    const auto& s=_slots.at(index); std::vector<ID3D12Resource*> result;
    for(auto& b:s.Buffers) result.push_back(b.Data.Get());
    for(UINT i=0;i<4;++i) result.push_back(s.Buffers[i].Source.Get());
    for(auto& c:s.Color) result.push_back(c->GetResource()->D3DResource.Get());
    result.push_back(s.Depth.Get()); result.push_back(target);
    result.push_back(s.ImageReadback.Get()); result.push_back(s.SceneReadback.Get());
    result.push_back(s.Geometry.VertexResource?s.Geometry.VertexResource:s.Buffers[0].Data.Get());
    result.push_back(s.Geometry.IndexResource?s.Geometry.IndexResource:s.Buffers[1].Data.Get());
    result.insert(result.end(),s.SkinBindings.begin(),s.SkinBindings.end());
    return result;
}
void SceneGeometry::Record(UINT index,Id op,UINT bucket,ID3D12GraphicsCommandList* cmd,ID3D12Resource* target) const {
    const auto& s=_slots.at(index); const auto& b=s.Buffers;
    if(op==0) {
        for(const auto& skin:s.Skin) cmd->CopyBufferRegion(skin.Resources[1],0,skin.Resources[0],0,UINT64(skin.BoneCount)*sizeof(GDX12SkinPalette));
        return;
    }
    if(op==1) { for(const auto& skin:s.Skin) skin.Record(cmd,s.Geometry.Vertices.BufferLocation); return; }
    op-=2;
    if(op==0) {
        // Four disjoint buckets of each destination share one packet prologue.
        for(UINT i=0;i<4;++i) {
            const UINT stride=i==0?sizeof(GDX12InstanceData):i==1?sizeof(GDX12IndirectDrawArgs):i==2?sizeof(GDX12MaterialConstants):sizeof(GDX12TransformConstants);
            const UINT count=UINT(b[i].Bytes/stride),begin=count*bucket/4,end=count*(bucket+1)/4;
            for(UINT element=begin;element<end;++element)
                cmd->CopyBufferRegion(b[i].Data.Get(),UINT64(element)*stride,b[i].Source.Get(),UINT64(element)*stride,stride);
        }
        return;
    }
    ID3D12DescriptorHeap* heap=_resources.SRV_UAV_Heap->GetHeap().Get(); cmd->SetDescriptorHeaps(1,&heap);
    if(op==1) {
        cmd->SetComputeRootSignature(_clearRoot->GetRootSignature().Get()); cmd->SetPipelineState(_clear.Get());
        cmd->SetComputeRootDescriptorTable(_clearRoot->GetURootParamIndex(0),b[5].Uav.GPUHandle);
        cmd->SetComputeRootDescriptorTable(_clearRoot->GetURootParamIndex(1),b[7].Uav.GPUHandle); cmd->Dispatch(1,1,1);
    } else if(op==2) {
        cmd->SetComputeRootSignature(_cullRoot->GetRootSignature().Get()); cmd->SetPipelineState(_cull.Get());
        cmd->SetComputeRootConstantBufferView(_cullRoot->GetBRootParamIndex(0),s.Camera);
        for(UINT i=0;i<3;++i) cmd->SetComputeRootDescriptorTable(_cullRoot->GetTRootParamIndex(i),b[i].Srv.GPUHandle);
        for(UINT i=0;i<4;++i) cmd->SetComputeRootDescriptorTable(_cullRoot->GetURootParamIndex(i),b[4+i].Uav.GPUHandle);
        if(s.Draws) cmd->Dispatch((s.Draws+63)/64,1,1);
    } else if(op==3 || op==4) {
        const bool depth=op==3; cmd->SetGraphicsRootSignature(_drawRoot->GetRootSignature().Get()); cmd->SetPipelineState(depth?_depth.Get():_gbuffer.Get());
        cmd->SetGraphicsRootConstantBufferView(_drawRoot->GetBRootParamIndex(1),s.Main);
        cmd->SetGraphicsRootConstantBufferView(_drawRoot->GetBRootParamIndex(2),s.Camera);
        const UINT buffers[]={2,3,0}; for(UINT i=0;i<3;++i) cmd->SetGraphicsRootDescriptorTable(_drawRoot->GetTRootParamIndex(i),b[buffers[i]].Srv.GPUHandle);
        cmd->SetGraphicsRootDescriptorTable(_drawRoot->GetTRootParamIndex(3),_resources.SRV_UAV_Heap->GetGPUHandle(Texture2D_StartIndex));
        const auto geom=_drawRoot->GetGeometryRootParamIndex();
        // Valid bindings even for branches unused by static geometry.
        const auto vertex=s.Geometry.Vertices.BufferLocation;
        cmd->SetGraphicsRootShaderResourceView(geom,vertex?vertex:b[0].Data->GetGPUVirtualAddress());
        cmd->SetGraphicsRootShaderResourceView(geom+1,b[0].Data->GetGPUVirtualAddress());
        D3D12_VIEWPORT vp{0,0,float(_width),float(_height),0,1}; D3D12_RECT rect{0,0,LONG(_width),LONG(_height)};
        cmd->RSSetViewports(1,&vp); cmd->RSSetScissorRects(1,&rect);
        D3D12_CPU_DESCRIPTOR_HANDLE rt[5]; for(UINT i=0;i<5;++i) rt[i]=s.Color[i]->GetRTV()->CPUHandle;
        const auto dsv=depth?s.Dsv.CPUHandle:s.ReadDepth.CPUHandle; cmd->OMSetRenderTargets(depth?0:5,rt,FALSE,&dsv);
        if(depth) cmd->ClearDepthStencilView(dsv,D3D12_CLEAR_FLAG_DEPTH,0,0,0,nullptr);
        else { const float clear[4]={}; for(auto handle:rt) cmd->ClearRenderTargetView(handle,clear,0,nullptr); }
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        if(s.Draws && s.Geometry.VertexResource) {
            cmd->IASetVertexBuffers(0,1,&s.Geometry.Vertices); cmd->IASetIndexBuffer(&s.Geometry.Indices);
            cmd->ExecuteIndirect(_signature.Get(),s.Draws,b[4].Data.Get(),0,b[5].Data.Get(),0);
        }
    } else if(op==5) cmd->CopyResource(target,s.Color[0]->GetResource()->D3DResource.Get());
    else if(op==6) {
        for(UINT i=0;i<8;++i) cmd->CopyBufferRegion(s.SceneReadback.Get(),s.Offsets[i],b[i].Data.Get(),0,b[i].Bytes);
        UINT64 offset=s.SkinOffset;
        for(const auto& skin:s.Skin) {
            const UINT64 bytes=UINT64(skin.VertexCount)*sizeof(Vertex);
            cmd->CopyBufferRegion(s.SceneReadback.Get(),offset,s.Geometry.VertexResource,UINT64(skin.OutputVertex)*sizeof(Vertex),bytes); offset+=bytes;
        }
        for(UINT i=0;i<6;++i) {
            D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource=s.ImageReadback.Get(); dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint=s.Footprints[i];
            D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource=i<5?s.Color[i]->GetResource()->D3DResource.Get():s.Depth.Get(); src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            cmd->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        }
    }
}
std::vector<uint8_t> SceneGeometry::Readback(UINT index,const std::string& stem) const {
    const auto& s=_slots.at(index); void* data=nullptr; D3D12_RANGE noWrite{};
    D3D12_RANGE sceneRange{0,SIZE_T(s.SceneBytes)};
    ThrowIfFailed(s.SceneReadback->Map(0,&sceneRange,&data));
    const auto* bytes=static_cast<const uint8_t*>(data);
    const auto count=*reinterpret_cast<const UINT*>(bytes+s.Offsets[5]);
    const auto transparent=*reinterpret_cast<const UINT*>(bytes+s.Offsets[7]);
    if(count>s.Draws || transparent>s.Draws) throw std::runtime_error("Invalid GPU visible count");
    // Compare only the valid command prefix, and normalize atomic append order.
    std::vector<uint8_t> result(bytes,bytes+s.Offsets[4]);
    std::vector<GDX12IndirectDrawArgs> commands(count);
    std::memcpy(commands.data(),bytes+s.Offsets[4],commands.size()*sizeof(commands[0]));
    std::sort(commands.begin(),commands.end(),[](auto& a,auto& b){return a.InstanceID<b.InstanceID;});
    auto first=reinterpret_cast<const uint8_t*>(commands.data());
    result.insert(result.end(),first,first+commands.size()*sizeof(commands[0]));
    for(UINT value:{count,transparent}) { const auto* p=reinterpret_cast<const uint8_t*>(&value); result.insert(result.end(),p,p+4); }
    result.insert(result.end(),bytes+s.SkinOffset,bytes+s.SceneBytes);
    if(!stem.empty()) { std::ofstream out(stem+"-skin.bin",std::ios::binary); out.write(reinterpret_cast<const char*>(bytes+s.SkinOffset),s.SceneBytes-s.SkinOffset); }
    if(!stem.empty()) { std::ofstream out(stem+"-scene.bin",std::ios::binary); out.write(reinterpret_cast<const char*>(bytes),s.SceneBytes); }
    s.SceneReadback->Unmap(0,&noWrite);
    D3D12_RANGE imageRange{0,SIZE_T(s.ImageBytes)}; ThrowIfFailed(s.ImageReadback->Map(0,&imageRange,&data)); bytes=static_cast<const uint8_t*>(data);
    for(UINT i=0;i<6;++i) {
        const auto& fp=s.Footprints[i]; const UINT pixelBytes=i==0 || i==1 || i==5?4:8;
        for(UINT y=0;y<_height;++y) {
            const auto* row=bytes+fp.Offset+UINT64(y)*fp.Footprint.RowPitch;
            result.insert(result.end(),row,row+_width*pixelBytes);
        }
    }
    if(!stem.empty()) {
        std::ofstream out(stem+"-albedo.ppm",std::ios::binary); out<<"P6\n"<<_width<<' '<<_height<<"\n255\n";
        for(UINT y=0;y<_height;++y) for(UINT x=0;x<_width;++x) out.write(reinterpret_cast<const char*>(bytes+s.Footprints[0].Offset+UINT64(y)*s.Footprints[0].Footprint.RowPitch+x*4),3);
        std::ofstream raw(stem+"-images.bin",std::ios::binary); raw.write(reinterpret_cast<const char*>(bytes),s.ImageBytes);
        std::ofstream meta(stem+"-metrics.txt"); meta<<"draws="<<s.Draws<<" opaque="<<count<<" transparent="<<transparent<<"\n";
        UINT covered=0; const auto& fp=s.Footprints[5];
        for(UINT y=0;y<_height;++y) for(UINT x=0;x<_width;++x) covered+=*reinterpret_cast<const float*>(bytes+fp.Offset+UINT64(y)*fp.Footprint.RowPitch+x*4)>0;
        meta<<"depth_covered_pixels="<<covered<<"\n";
    }
    s.ImageReadback->Unmap(0,&noWrite); return result;
}
}

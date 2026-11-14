#include "SceneShadows.h"
#include "Engine.RendererDX12/GDX12TextureResource.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include "Engine.RendererDX12/GDX12ShaderCompiler.h"
#include <fstream>
#include <algorithm>
#include <filesystem>

namespace Meta {
SceneShadows::SceneShadows(GDX12DeviceResources& r):_resources(r),_slots(r.FrameConstants.size()) {
    GDX12RootSignatureDesc pack;pack.Constants={4};pack.NumSingleSRVSlots=1;pack.NumSingleUAVSlots=1;
    _packRoot=std::make_unique<GDX12RootSignature>(r.Device,pack);
    _packPipeline=GDX12RenderHelpers::CreateComputePipeline(r.Device,_packRoot.get(),(std::filesystem::path(PROJECT_DIR)/"Shaders/ShadowAtlasCopy.hlsl").c_str(),"CS");
    // The same local-shadow frustum and static/dynamic filters also consume the
    // four matrices produced by BuildShadowCascades. Each view owns its constants.
    GDX12RootSignatureDesc root; root.Constants={3}; root.NumSingleSRVSlots=4; root.NumSingleUAVSlots=2;
    _computeRoot=std::make_unique<GDX12RootSignature>(r.Device,root);
    for(UINT i=0;i<2;++i) {
        D3D_SHADER_MACRO defines[]={{"SHADOW_CULLING","1"},{"STATIC_ONLY",i==0?"1":"0"},{nullptr,nullptr}};
        _cull[i]=GDX12RenderHelpers::CreateComputePipeline(r.Device,_computeRoot.get(),SHADERS_FOLDER "LocalShadowPass.hlsl","CS",defines);
        if(i==0) _clear=GDX12RenderHelpers::CreateComputePipeline(r.Device,_computeRoot.get(),SHADERS_FOLDER "LocalShadowPass.hlsl","ClearCS",defines);
    }
    root={}; root.Constants={2}; root.NumSingleSRVSlots=4; root.InstancedGeometry=true;
    root.StaticSamplers=GetStaticSamplers(); root.SRVRanges.emplace_back(Texture2D_RangeLength);
    _graphicsRoot=std::make_unique<GDX12RootSignature>(r.Device,root);
    _signature=GDX12RenderHelpers::CreateIndexedDrawSignature(r.Device,_graphicsRoot.get());
    auto& compiler=GDX12ShaderCompiler::GetInstance();
    D3D_SHADER_MACRO directInstances[]={{"SHADOW_UNBATCHED","1"},{nullptr,nullptr}};
    auto vs=compiler.CompileShader(r.Device,SHADERS_FOLDER "LocalShadowPass.hlsl",directInstances,"VS","vs");
    auto ps=compiler.CompileShader(r.Device,SHADERS_FOLDER "LocalShadowPass.hlsl",nullptr,"PS","ps");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC p{}; p.pRootSignature=_graphicsRoot->GetRootSignature().Get();
    p.InputLayout={r.InputLayouts.at("Default").data(),UINT(r.InputLayouts.at("Default").size())};
    p.VS={vs->GetBufferPointer(),vs->GetBufferSize()}; p.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
    p.RasterizerState=CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT); p.RasterizerState.FrontCounterClockwise=TRUE;
    p.RasterizerState.SlopeScaledDepthBias=1.5f; p.BlendState=CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    p.DepthStencilState=CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT); p.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_LESS_EQUAL;
    p.SampleMask=UINT_MAX; p.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; p.DSVFormat=DXGI_FORMAT_D32_FLOAT; p.SampleDesc.Count=1;
    for(UINT i=0;i<2;++i) { p.RasterizerState.DepthBias=i==0?500:250; ThrowIfFailed(r.Device->GetDevice()->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&_draw[i]))); }
}
void SceneShadows::Freeze(UINT index,const std::array<D3D12_GPU_DESCRIPTOR_HANDLE,4>& scene,UINT draws,GDX12GeometryBuffer::RecordingBinding geometry) {
    auto& s=_slots.at(index); s.Scene=scene; s.Geometry=geometry; s.Draws=draws;
    struct Input { GDX12LocalLightConstants Constants; UINT Resolution; bool Sun; };
    std::vector<Input> inputs;
    if((_resources.Lighting.SunShadowFlags&1) && _resources.ShadowQuality.SunResolution) {
        for(UINT i=0;i<4;++i) { GDX12LocalLightConstants light; light.ShadowViewProj[0]=_resources.ShadowSettings.ViewProj[i]; inputs.push_back({light,_resources.ShadowQuality.SunResolution,true}); }
    }
    if(_resources.ShadowQuality.LocalResolution) for(const auto& light:_resources.LocalLights) {
        if(!(light.ShadowFlags&1)) continue;
        for(UINT face=0;face<LocalShadowFaceCount(light);++face) {
            auto constants=light; constants.ShadowViewProj[0]=light.ShadowViewProj[face]; inputs.push_back({constants,_resources.ShadowQuality.LocalResolution,false});
        }
    }
    s.Views.resize(inputs.size());
    for(UINT i=0;i<inputs.size();++i) {
        if(!s.Views[i]) s.Views[i]=std::make_unique<View>(); auto& v=*s.Views[i]; const auto& input=inputs[i]; v.Sun=input.Sun;
        if(!v.Constants) {
            v.Constants=std::make_unique<GDX12UploadBuffer<GDX12LocalLightConstants>>(_resources.Device,1,EBufferType::Upload,false);
            v.Constants->CreateSRV(_resources.SRV_UAV_Heap.get(),_resources.SRV_UAV_Heap->GetAvailableIndex(105000));
        }
        v.Constants->CopyData(0,input.Constants);
        v.ViewProjection=Matrix(input.Constants.ShadowViewProj[0]).Transpose();
        for(UINT part=0;part<2;++part) {
            if(!v.Commands[part] || v.Commands[part]->GetElementCount()<(std::max)(draws,1u)) {
                v.Commands[part]=std::make_unique<GDX12UploadBuffer<GDX12IndirectDrawArgs>>(_resources.Device,(std::max)(draws,1u),EBufferType::Default,false);
                v.Commands[part]->CreateUAV(_resources.SRV_UAV_Heap.get(),_resources.SRV_UAV_Heap->GetAvailableIndex(105000));
            }
            if(!v.Counts[part]) {
                v.Counts[part]=std::make_unique<GDX12UploadBuffer<UINT>>(_resources.Device,1,EBufferType::Default,false);
                v.Counts[part]->CreateUAV(_resources.SRV_UAV_Heap.get(),_resources.SRV_UAV_Heap->GetAvailableIndex(105000));
            }
        }
        if(v.Resolution!=input.Resolution) {
            v.Resolution=input.Resolution; auto desc=CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32_TYPELESS,v.Resolution,v.Resolution,1,1,1,0,D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
            D3D12_CLEAR_VALUE clear{}; clear.Format=DXGI_FORMAT_D32_FLOAT; clear.DepthStencil.Depth=1;
            v.Depth=_resources.Device->CreateResource(D3D12_HEAP_TYPE_DEFAULT,desc,D3D12_RESOURCE_STATE_COMMON,&clear);
            v.StaticDepth=_resources.Device->CreateResource(D3D12_HEAP_TYPE_DEFAULT,desc,D3D12_RESOURCE_STATE_COMMON,&clear);
            D3D12_DEPTH_STENCIL_VIEW_DESC dsv{}; dsv.Format=DXGI_FORMAT_D32_FLOAT; dsv.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2D;
            auto* heap=_resources.DSVHeap.get(); v.Dsv.InitAsDSV(v.Depth.Get(),&dsv,heap,v.Dsv.HeapIndex==UINT_MAX?heap->GetAvailableIndex():v.Dsv.HeapIndex);
            v.StaticDsv.InitAsDSV(v.StaticDepth.Get(),&dsv,heap,v.StaticDsv.HeapIndex==UINT_MAX?heap->GetAvailableIndex():v.StaticDsv.HeapIndex);
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=DXGI_FORMAT_R32_FLOAT;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2D.MipLevels=1;
            auto* views=_resources.SRV_UAV_Heap.get();
            v.DepthSrv.InitAsSRV(v.Depth.Get(),&srv,views,v.DepthSrv.HeapIndex==UINT_MAX?views->GetAvailableIndex(105000):v.DepthSrv.HeapIndex);
            v.StaticSrv.InitAsSRV(v.StaticDepth.Get(),&srv,views,v.StaticSrv.HeapIndex==UINT_MAX?views->GetAvailableIndex(105000):v.StaticSrv.HeapIndex);
            _resources.Device->GetDevice()->GetCopyableFootprints(&desc,0,1,0,&v.Footprint,nullptr,nullptr,&v.DepthBytes);
            v.Readback=_resources.Device->CreateResource(D3D12_HEAP_TYPE_READBACK,CD3DX12_RESOURCE_DESC::Buffer(v.DepthBytes+8),D3D12_RESOURCE_STATE_COPY_DEST);
        }
    }
}
void SceneShadows::Append(Graph& g,UINT index) {
    auto& s=_slots.at(index); s.FirstOperation=Id(g.Operations.size()); s.Repeated.clear();
    const State shader=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    for(UINT i=0;i<s.Views.size();++i) {
        const Id base=Id(g.Resources.size()); const std::string name=(s.Views[i]->Sun?"cascade ":"local face ")+std::to_string(i);
        s.Views[i]->DepthId=base+5;
        g.Resources.push_back({name+" constants",D3D12_RESOURCE_STATE_GENERIC_READ,D3D12_RESOURCE_STATE_GENERIC_READ,false,false,Heap::Upload});
        for(UINT part=0;part<2;++part) { g.Resources.push_back({name+" commands",0,0,true,true}); g.Resources.push_back({name+" count",0,0,true,true}); }
        g.Resources.push_back({name+" depth",0,0}); g.Resources.push_back({name+" readback",D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_DEST,false,false,Heap::Readback});
        g.Operations.push_back({name+" reset",{{base+2,Mode::Write,UnorderedAccess},{base+4,Mode::Write,UnorderedAccess}}});
        g.Operations.push_back({name+" cull static/dynamic",{{0,Mode::Read,shader},{1,Mode::Read,shader},{2,Mode::Read,shader},{base,Mode::Read,D3D12_RESOURCE_STATE_GENERIC_READ},
            {base+1,Mode::Write,UnorderedAccess},{base+2,Mode::ReadWrite,UnorderedAccess},{base+3,Mode::Write,UnorderedAccess},{base+4,Mode::ReadWrite,UnorderedAccess}},{},false,2});
        g.Operations.push_back({name+" ExecuteIndirect",{{0,Mode::Read,shader},{2,Mode::Read,shader},{3,Mode::Read,shader},{base,Mode::Read,D3D12_RESOURCE_STATE_GENERIC_READ},
            {21,Mode::Read,D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER|D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE},{22,Mode::Read,D3D12_RESOURCE_STATE_INDEX_BUFFER},
            {base+1,Mode::Read,D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT},{base+2,Mode::Read,D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT},
            {base+3,Mode::Read,D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT},{base+4,Mode::Read,D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT},{base+5,Mode::Write,D3D12_RESOURCE_STATE_DEPTH_WRITE}}});
        g.Operations.push_back({name+" readback",{{base+2,Mode::Read,D3D12_RESOURCE_STATE_COPY_SOURCE},{base+4,Mode::Read,D3D12_RESOURCE_STATE_COPY_SOURCE},
            {base+5,Mode::Read,D3D12_RESOURCE_STATE_COPY_SOURCE},{base+6,Mode::Write,D3D12_RESOURCE_STATE_COPY_DEST}}});
    }
    for(UINT i=0;i<s.Views.size();++i) {
        const Id id=Id(g.Resources.size());g.Resources.push_back({"static lighting shadow "+std::to_string(i),0,0});
        g.Operations[s.FirstOperation+i*4+2].Accesses.push_back({id,Mode::Write,D3D12_RESOURCE_STATE_DEPTH_WRITE});
    }
}
std::vector<ID3D12Resource*> SceneShadows::Resources(UINT index) const {
    std::vector<ID3D12Resource*> result;
    for(const auto& view:_slots.at(index).Views) {
        const auto& v=*view; result.push_back(v.Constants->GetResource().D3DResource.Get());
        for(UINT i=0;i<2;++i) { result.push_back(v.Commands[i]->GetResource().D3DResource.Get()); result.push_back(v.Counts[i]->GetResource().D3DResource.Get()); }
        result.push_back(v.Depth.Get()); result.push_back(v.Readback.Get());
    }
    for(const auto& view:_slots.at(index).Views)result.push_back(view->StaticDepth.Get());
    return result;
}
bool SceneShadows::Record(UINT index,Id operation,UINT bucket,ID3D12GraphicsCommandList* cmd) const {
    const auto& s=_slots.at(index);
    if(s.PackedWork.Record(operation,cmd))return true;
    for(auto repeat:s.Repeated) if(repeat.first==operation) operation=repeat.second;
    if(operation<s.FirstOperation || operation>=s.FirstOperation+s.Views.size()*4) return false;
    const auto& v=*s.Views[(operation-s.FirstOperation)/4]; const UINT stage=(operation-s.FirstOperation)%4;
    ID3D12DescriptorHeap* heap=_resources.SRV_UAV_Heap->GetHeap().Get(); cmd->SetDescriptorHeaps(1,&heap);
    if(stage<2) {
        cmd->SetComputeRootSignature(_computeRoot->GetRootSignature().Get()); const UINT constants[]={0,0,s.Draws}; cmd->SetComputeRoot32BitConstants(0,3,constants,0);
        for(UINT i=0;i<3;++i) cmd->SetComputeRootDescriptorTable(_computeRoot->GetTRootParamIndex(i),s.Scene[i]);
        cmd->SetComputeRootDescriptorTable(_computeRoot->GetTRootParamIndex(3),v.Constants->GetSRV()->GPUHandle);
        // Each callback owns its descriptor bindings and command-list state.
        for(UINT part=0;part<2;++part) {
            if(stage==1 && part!=bucket) continue;
            cmd->SetComputeRootDescriptorTable(_computeRoot->GetURootParamIndex(0),v.Commands[part]->GetUAV()->GPUHandle);
            cmd->SetComputeRootDescriptorTable(_computeRoot->GetURootParamIndex(1),v.Counts[part]->GetUAV()->GPUHandle);
            cmd->SetPipelineState(stage==0?_clear.Get():_cull[part].Get()); if(stage==0 || s.Draws) cmd->Dispatch(stage==0?1:(s.Draws+63)/64,1,1);
        }
    } else if(stage==2) {
        cmd->SetGraphicsRootSignature(_graphicsRoot->GetRootSignature().Get()); cmd->SetPipelineState(_draw[v.Sun?0:1].Get());
        const UINT constants[]={0,0}; cmd->SetGraphicsRoot32BitConstants(0,2,constants,0);
        const UINT bindings[]={2,3,0}; for(UINT i=0;i<3;++i) cmd->SetGraphicsRootDescriptorTable(_graphicsRoot->GetTRootParamIndex(i),s.Scene[bindings[i]]);
        cmd->SetGraphicsRootDescriptorTable(_graphicsRoot->GetTRootParamIndex(3),v.Constants->GetSRV()->GPUHandle);
        cmd->SetGraphicsRootDescriptorTable(_graphicsRoot->GetTRootParamIndex(4),_resources.SRV_UAV_Heap->GetGPUHandle(Texture2D_StartIndex));
        const auto root=_graphicsRoot->GetGeometryRootParamIndex(); cmd->SetGraphicsRootShaderResourceView(root,s.Geometry.Vertices.BufferLocation);
        cmd->SetGraphicsRootShaderResourceView(root+1,s.Geometry.Vertices.BufferLocation);
        D3D12_VIEWPORT vp{0,0,float(v.Resolution),float(v.Resolution),0,1}; D3D12_RECT rect{0,0,LONG(v.Resolution),LONG(v.Resolution)};
        cmd->RSSetViewports(1,&vp); cmd->RSSetScissorRects(1,&rect); cmd->OMSetRenderTargets(0,nullptr,FALSE,&v.Dsv.CPUHandle);
        cmd->ClearDepthStencilView(v.Dsv.CPUHandle,D3D12_CLEAR_FLAG_DEPTH,1,0,0,nullptr);
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST); cmd->IASetVertexBuffers(0,1,&s.Geometry.Vertices); cmd->IASetIndexBuffer(&s.Geometry.Indices);
        for(UINT part=0;part<2;++part) if(s.Draws) cmd->ExecuteIndirect(_signature.Get(),s.Draws,v.Commands[part]->GetResource().D3DResource.Get(),0,v.Counts[part]->GetResource().D3DResource.Get(),0);
        // Static-only probe receivers retain static-only local shadowing.
        // Grass is dynamic in the existing reflection implementation.
        cmd->OMSetRenderTargets(0,nullptr,FALSE,&v.StaticDsv.CPUHandle);
        cmd->ClearDepthStencilView(v.StaticDsv.CPUHandle,D3D12_CLEAR_FLAG_DEPTH,1,0,0,nullptr);
        if(s.Draws)cmd->ExecuteIndirect(_signature.Get(),s.Draws,v.Commands[0]->GetResource().D3DResource.Get(),0,v.Counts[0]->GetResource().D3DResource.Get(),0);
    } else {
        D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource=v.Readback.Get(); dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint=v.Footprint;
        D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource=v.Depth.Get(); src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; cmd->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        for(UINT part=0;part<2;++part) cmd->CopyBufferRegion(v.Readback.Get(),v.DepthBytes+part*4,v.Counts[part]->GetResource().D3DResource.Get(),0,4);
    }
    return true;
}
std::vector<GeometryTarget> SceneShadows::Targets(UINT slot) const {
    std::vector<GeometryTarget> result; UINT i=0;
    for(const auto& view:_slots.at(slot).Views) {
        const auto& v=*view; GeometryTarget target;
        target.View={GDX12GeometryStage::Shadow,v.ViewProjection,Vector3::Zero}; target.View.Key=(2ull<<60)|i++;
        target.View.FrameSlot=slot; target.View.DepthFormat=DXGI_FORMAT_D32_FLOAT;
        target.Depth=v.DepthId; target.Dsv=v.Dsv.CPUHandle; target.Width=target.Height=v.Resolution; result.push_back(target);
    }
    return result;
}
void SceneShadows::RepeatReadbacks(Graph& g,UINT slot) {
    auto& s=_slots.at(slot);
    for(UINT i=0;i<s.Views.size();++i) {
        const Id original=s.FirstOperation+i*4+3; s.Repeated.push_back({Id(g.Operations.size()),original}); g.Operations.push_back(g.Operations[original]);
    }
}
std::vector<uint8_t> SceneShadows::Readback(UINT index,const std::string& stem) const {
    std::vector<uint8_t> result; UINT viewIndex=0;
    for(const auto& view:_slots.at(index).Views) {
        const auto& v=*view; void* mapped=nullptr; D3D12_RANGE range{0,SIZE_T(v.DepthBytes+8)},noWrite{}; ThrowIfFailed(v.Readback->Map(0,&range,&mapped));
        const auto* bytes=static_cast<const uint8_t*>(mapped); UINT covered=0;
        std::ofstream image; if(!stem.empty()) { image.open(stem+"-shadow-"+std::to_string(viewIndex)+".pgm",std::ios::binary); image<<"P5\n"<<v.Resolution<<' '<<v.Resolution<<"\n255\n"; }
        for(UINT y=0;y<v.Resolution;++y) {
            const auto* row=bytes+UINT64(y)*v.Footprint.Footprint.RowPitch;
            result.insert(result.end(),row,row+v.Resolution*4);
            for(UINT x=0;x<v.Resolution;++x) { float depth; std::memcpy(&depth,row+x*4,4); covered+=depth<1; if(image) image.put(depth<1?char(255):char(0)); }
        }
        result.insert(result.end(),bytes+v.DepthBytes,bytes+v.DepthBytes+8);
        if(!stem.empty()) { std::ofstream raw(stem+"-shadow-"+std::to_string(viewIndex)+".bin",std::ios::binary); raw.write(reinterpret_cast<const char*>(bytes),v.DepthBytes+8); }
        if(!stem.empty()) { std::ofstream meta(stem+"-shadow-"+std::to_string(viewIndex)+".txt"); const auto* counts=reinterpret_cast<const UINT*>(bytes+v.DepthBytes); meta<<"sun="<<v.Sun<<" covered="<<covered<<" static="<<counts[0]<<" dynamic="<<counts[1]<<"\n"; }
        v.Readback->Unmap(0,&noWrite); ++viewIndex;
    }
    return result;
}
void SceneShadows::AppendLighting(Graph &graph, std::vector<ID3D12Resource *> &bindings, UINT index)
{
    auto &s = _slots.at(index);
    s.PackedWork.Commands.clear();
    UINT slices = 1;
    for (const auto &light : _resources.LocalLights)
        if ((light.ShadowFlags & 1) && light.ShadowFirstSlice != UINT_MAX)
            slices = (std::max)(slices, light.ShadowFirstSlice + LocalShadowFaceCount(light));
    const UINT sunSize =
        (_resources.Lighting.SunShadowFlags & 1) ? (std::max)(1u, _resources.ShadowQuality.SunResolution * 2) : 1;
    const UINT localSize = (std::max)(1u, _resources.ShadowQuality.LocalResolution);
    const auto ensure = [&](PackedDepth &output, UINT width, UINT height, UINT count, bool array) {
        if (output.Width == width && output.Height == height && output.Slices == count)
            return;
        output.Width = width;
        output.Height = height;
        output.Slices = count;
        GDX12TextureDesc desc;
        desc.Width = width;
        desc.Height = height;
        desc.Format = desc.SRVDesc.Format = desc.UAVDesc.Format = DXGI_FORMAT_R32_FLOAT;
        desc.SRV_UAV_Heap = _resources.SRV_UAV_Heap.get();
        desc.CreateUAV = true;
        desc.SRVHeapIndex = output.Texture.IsInitialized() ? output.Texture.GetSRV()->HeapIndex
                                                           : desc.SRV_UAV_Heap->GetAvailableIndex(105000);
        desc.UAVHeapIndex = output.Texture.IsInitialized() ? output.Texture.GetUAV()->HeapIndex
                                                           : desc.SRV_UAV_Heap->GetAvailableIndex(105000);
        desc.SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if (!array)
        {
            desc.SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            desc.SRVDesc.Texture2D.MipLevels = 1;
        }
        else
        {
            desc.SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            desc.SRVDesc.Texture2DArray.MipLevels = 1;
            desc.SRVDesc.Texture2DArray.ArraySize = count;
        }
        desc.UAVDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
        desc.UAVDesc.Texture2DArray.ArraySize = count;
        desc.ExternalResource = _resources.Device->CreateResource(
            D3D12_HEAP_TYPE_DEFAULT,
            CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32_FLOAT, width, height, UINT16(count), 1, 1, 0,
                                         D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
            D3D12_RESOURCE_STATE_COMMON);
        output.Texture.Initialize(desc);
        D3D12_DESCRIPTOR_HEAP_DESC clearHeap{};
        clearHeap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        clearHeap.NumDescriptors = 1;
        output.ClearHeap.Reset();
        ThrowIfFailed(
            _resources.Device->GetDevice()->CreateDescriptorHeap(&clearHeap, IID_PPV_ARGS(&output.ClearHeap)));
        _resources.Device->GetDevice()->CreateUnorderedAccessView(
            desc.ExternalResource.Get(), nullptr, &desc.UAVDesc,
            output.ClearHeap->GetCPUDescriptorHandleForHeapStart());
    };
    ensure(s.SunAtlas, sunSize, sunSize, 1, false);
    ensure(s.Local, localSize, localSize, slices, true);
    ensure(s.StaticLocal, localSize, localSize, slices, true);
    const auto resource = [&](ID3D12Resource *r) {
        return NativeWork::ResourceFor(graph, bindings, r, "lighting shadow atlas");
    };
    auto *heap = _resources.SRV_UAV_Heap->GetHeap().Get();
    for (auto *output : {&s.SunAtlas, &s.Local, &s.StaticLocal})
    {
        auto *target = output->Texture.GetResource()->D3DResource.Get();
        const auto gpu = output->Texture.GetUAV()->GPUHandle;
        const auto cpu = output->ClearHeap->GetCPUDescriptorHandleForHeapStart();
        s.PackedWork.Add(graph, "initialize lighting shadow fallback",
                         {{resource(target), Mode::Write, UnorderedAccess}}, [=](auto *cmd) {
                             cmd->SetDescriptorHeaps(1, &heap);
                             const float white[4] = {1, 1, 1, 1};
                             cmd->ClearUnorderedAccessViewFloat(gpu, cpu, target, white, 0, nullptr);
                         });
    }
    const auto copy = [&](const View &view, GDX12Descriptor &srv, PackedDepth &output, UINT slice, UINT x, UINT y,
                          bool statics) {
        auto *source = statics ? view.StaticDepth.Get() : view.Depth.Get();
        auto *target = output.Texture.GetResource()->D3DResource.Get();
        auto *root = _packRoot.get();
        auto *pso = _packPipeline.Get();
        const auto input = srv.GPUHandle, write = output.Texture.GetUAV()->GPUHandle;
        const UINT size = view.Resolution;
        // A lossless layout copy only. The source contains the actual U4 mesh /
        // grass shadow draws; no shadow evaluation is replaced by this kernel.
        s.PackedWork.Add(graph, statics ? "pack static probe shadow" : "pack rendered lighting shadow",
                         {{resource(source), Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE},
                          {resource(target), Mode::Write, UnorderedAccess}},
                         [=](auto *cmd) {
                             cmd->SetDescriptorHeaps(1, &heap);
                             cmd->SetComputeRootSignature(root->GetRootSignature().Get());
                             cmd->SetPipelineState(pso);
                             const UINT c[] = {size, x, y, slice};
                             cmd->SetComputeRoot32BitConstants(0, 4, c, 0);
                             cmd->SetComputeRootDescriptorTable(root->GetTRootParamIndex(0), input);
                             cmd->SetComputeRootDescriptorTable(root->GetURootParamIndex(0), write);
                             cmd->Dispatch((size + 7) / 8, (size + 7) / 8, 1);
                         });
    };
    UINT sun = 0, local = 0;
    std::vector<UINT> localSlices;
    for (const auto &light : _resources.LocalLights)
        if (light.ShadowFlags & 1)
            for (UINT face = 0; face < LocalShadowFaceCount(light); ++face)
                localSlices.push_back(light.ShadowFirstSlice + face);
    for (const auto &view : s.Views)
    {
        const UINT slice = view->Sun ? 0 : localSlices.at(local);
        const UINT x = view->Sun ? (sun & 1) * view->Resolution : 0, y = view->Sun ? (sun >> 1) * view->Resolution : 0;
        copy(*view, view->DepthSrv, view->Sun ? s.SunAtlas : s.Local, slice, x, y, false);
        if (!view->Sun)
        {
            copy(*view, view->StaticSrv, s.StaticLocal, slice, 0, 0, true);
            ++local;
        }
        else
            ++sun;
    }
}

} // namespace Meta

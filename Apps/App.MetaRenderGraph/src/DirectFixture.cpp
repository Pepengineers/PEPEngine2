#include "DirectFixture.h"
#include <d3dcompiler.h>
#include <cstring>
#include <stdexcept>
#pragma comment(lib,"d3dcompiler.lib")
namespace Meta {
namespace {
ComPtr<ID3DBlob> Shader(const char* source,const char* entry,const char* profile) {
    ComPtr<ID3DBlob> code,errors;
    const auto hr=D3DCompile(source,strlen(source),nullptr,nullptr,nullptr,entry,profile,D3DCOMPILE_ENABLE_STRICTNESS,0,&code,&errors);
    if(FAILED(hr) && errors) throw std::runtime_error(static_cast<const char*>(errors->GetBufferPointer()));
    ThrowIfFailed(hr); return code;
}
}
DirectFixture::DirectFixture(GDX12Device& d,uint32_t count):_device(d),_slots(count) {
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV; params[0].Descriptor.ShaderRegister=0;
    params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV; params[1].Descriptor.ShaderRegister=0;
    D3D12_ROOT_SIGNATURE_DESC root{2,params,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
    ComPtr<ID3DBlob> serialized,errors;
    ThrowIfFailed(D3D12SerializeRootSignature(&root,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&errors));
    ThrowIfFailed(d.GetDevice()->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&_root)));
    const auto cs=Shader("RWStructuredBuffer<uint> data:register(u0); [numthreads(4,1,1)] void main(uint3 id:SV_DispatchThreadID){data[id.x]=data[id.x]*2+1;}","main","cs_5_1");
    D3D12_COMPUTE_PIPELINE_STATE_DESC compute{}; compute.pRootSignature=_root.Get(); compute.CS={cs->GetBufferPointer(),cs->GetBufferSize()};
    ThrowIfFailed(d.GetDevice()->CreateComputePipelineState(&compute,IID_PPV_ARGS(&_compute)));
    const char* source=R"(
StructuredBuffer<uint> data:register(t0);
struct V { float4 position:SV_Position; nointerpolation float4 color:COLOR; };
V vs(uint id:SV_VertexID) { V o; float2 p[3]={float2(-1,-1),float2(-1,3),float2(3,-1)};
o.position=float4(p[id],0,1); o.color=float4(data[0]/255.0,0,1,1); return o; }
float4 ps(V v):SV_Target { return v.color; }
)";
    auto vs=Shader(source,"vs","vs_5_1"), ps=Shader(source,"ps","ps_5_1");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics{};
    graphics.pRootSignature=_root.Get(); graphics.VS={vs->GetBufferPointer(),vs->GetBufferSize()}; graphics.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
    graphics.BlendState=CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    graphics.RasterizerState=CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT); graphics.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
    graphics.DepthStencilState=CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT); graphics.DepthStencilState.DepthEnable=FALSE; graphics.DepthStencilState.StencilEnable=FALSE;
    graphics.SampleMask=UINT_MAX; graphics.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    graphics.NumRenderTargets=1; graphics.RTVFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM; graphics.SampleDesc.Count=1;
    ThrowIfFailed(d.GetDevice()->CreateGraphicsPipelineState(&graphics,IID_PPV_ARGS(&_graphics)));
    for(auto& s:_slots) {
        auto desc=CD3DX12_RESOURCE_DESC::Buffer(16);
        s.Upload=d.CreateResource(D3D12_HEAP_TYPE_UPLOAD,desc,D3D12_RESOURCE_STATE_GENERIC_READ);
        desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        s.Data=d.CreateResource(D3D12_HEAP_TYPE_DEFAULT,desc,D3D12_RESOURCE_STATE_COMMON);
        desc.Flags=D3D12_RESOURCE_FLAG_NONE;
        s.Readback=d.CreateResource(D3D12_HEAP_TYPE_READBACK,desc,D3D12_RESOURCE_STATE_COPY_DEST);
        s.A=d.CreateResource(D3D12_HEAP_TYPE_READBACK,desc,D3D12_RESOURCE_STATE_COPY_DEST);
        s.B=d.CreateResource(D3D12_HEAP_TYPE_READBACK,desc,D3D12_RESOURCE_STATE_COPY_DEST);
        s.Pixel=d.CreateResource(D3D12_HEAP_TYPE_READBACK,CD3DX12_RESOURCE_DESC::Buffer(256),D3D12_RESOURCE_STATE_COPY_DEST);
    }
}
void DirectFixture::Prepare(uint32_t index,uint32_t seed) {
    auto& s=_slots.at(index); s.Seed=seed; s.Used=true;
    void* mapped=nullptr; D3D12_RANGE read{0,0}; ThrowIfFailed(s.Upload->Map(0,&read,&mapped));
    for(uint32_t i=0;i<4;++i) static_cast<uint32_t*>(mapped)[i]=seed+i;
    s.Upload->Unmap(0,nullptr);
}
Schedule DirectFixture::Plan(State initial,State final) const {
    Graph g;
    g.Resources={{"data",D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COMMON,true},
        {"upload",D3D12_RESOURCE_STATE_GENERIC_READ,D3D12_RESOURCE_STATE_GENERIC_READ},
        {"result",D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_DEST},
        {"reader A",D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_DEST},
        {"reader B",D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_DEST},
        {"target",initial,final}, {"pixel",D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_DEST}};
    g.Operations={
        {"upload",{{0,Mode::Write,D3D12_RESOURCE_STATE_COPY_DEST},{1,Mode::Read,D3D12_RESOURCE_STATE_GENERIC_READ}},{},false,2},
        {"transform",{{0,Mode::ReadWrite,D3D12_RESOURCE_STATE_UNORDERED_ACCESS}}},
        {"same-state UAV transform",{{0,Mode::ReadWrite,D3D12_RESOURCE_STATE_UNORDERED_ACCESS}}},
        {"reader A",{{0,Mode::Read,D3D12_RESOURCE_STATE_COPY_SOURCE},{3,Mode::Write,D3D12_RESOURCE_STATE_COPY_DEST}}},
        {"reader B",{{0,Mode::Read,D3D12_RESOURCE_STATE_COPY_SOURCE},{4,Mode::Write,D3D12_RESOURCE_STATE_COPY_DEST}}},
        {"write after both readers",{{0,Mode::ReadWrite,D3D12_RESOURCE_STATE_UNORDERED_ACCESS}}},
        {"draw",{{0,Mode::Read,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE},{5,Mode::Write,D3D12_RESOURCE_STATE_RENDER_TARGET}},{},true},
        {"readback",{{0,Mode::Read,D3D12_RESOURCE_STATE_COPY_SOURCE},{2,Mode::Write,D3D12_RESOURCE_STATE_COPY_DEST},
            {5,Mode::Read,D3D12_RESOURCE_STATE_COPY_SOURCE},{6,Mode::Write,D3D12_RESOURCE_STATE_COPY_DEST}}},
        {"empty side effect",{},{7},true}};
    return Compile(g);
}
std::vector<ID3D12Resource*> DirectFixture::Resources(uint32_t i,ID3D12Resource* target) const {
    const auto& s=_slots.at(i); return {s.Data.Get(),s.Upload.Get(),s.Readback.Get(),s.A.Get(),s.B.Get(),target,s.Pixel.Get()};
}
void DirectFixture::Record(uint32_t i,Id op,uint32_t list,ID3D12GraphicsCommandList* cmd,
    ID3D12Resource* target,D3D12_CPU_DESCRIPTOR_HANDLE rtv,uint32_t width,uint32_t height) {
    auto& s=_slots.at(i);
    if(op==0) cmd->CopyBufferRegion(s.Data.Get(),list*8,s.Upload.Get(),list*8,8);
    else if(op==1 || op==2 || op==5) {
        cmd->SetComputeRootSignature(_root.Get()); cmd->SetPipelineState(_compute.Get());
        cmd->SetComputeRootUnorderedAccessView(0,s.Data->GetGPUVirtualAddress()); cmd->Dispatch(1,1,1);
    } else if(op==3 || op==4) cmd->CopyBufferRegion(op==3?s.A.Get():s.B.Get(),0,s.Data.Get(),0,16);
    else if(op==6) {
        cmd->SetGraphicsRootSignature(_root.Get()); cmd->SetPipelineState(_graphics.Get());
        cmd->SetGraphicsRootShaderResourceView(1,s.Data->GetGPUVirtualAddress());
        D3D12_VIEWPORT viewport{0,0,float(width),float(height),0,1}; D3D12_RECT rect{0,0,LONG(width),LONG(height)};
        cmd->RSSetViewports(1,&viewport); cmd->RSSetScissorRects(1,&rect); cmd->OMSetRenderTargets(1,&rtv,FALSE,nullptr);
        // D3D12MA may return non-zeroed memory. A full-screen draw does not
        // satisfy the initialization rule for a newly allocated render target.
        const float clear[4]={0,0,0,1};
        cmd->ClearRenderTargetView(rtv,clear,0,nullptr);
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST); cmd->DrawInstanced(3,1,0,0);
    } else if(op==7) {
        cmd->CopyBufferRegion(s.Readback.Get(),0,s.Data.Get(),0,16);
        D3D12_TEXTURE_COPY_LOCATION dest{}; dest.pResource=s.Pixel.Get(); dest.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dest.PlacedFootprint.Footprint={DXGI_FORMAT_R8G8B8A8_UNORM,1,1,1,256};
        D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource=target; src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_BOX box{width/2,height/2,0,width/2+1,height/2+1,1};
        cmd->CopyTextureRegion(&dest,0,0,0,&src,&box);
    }
}
void DirectFixture::Validate(uint32_t i) const {
    const auto& s=_slots.at(i); if(!s.Used) return;
    const auto check=[&](ID3D12Resource* resource,uint32_t multiplier,uint32_t add) {
        void* data=nullptr; D3D12_RANGE range{0,16}; ThrowIfFailed(resource->Map(0,&range,&data));
        bool valid=true;
        for(uint32_t j=0;j<4;++j) valid=valid && static_cast<uint32_t*>(data)[j]==(s.Seed+j)*multiplier+add;
        D3D12_RANGE written{0,0}; resource->Unmap(0,&written);
        if(!valid) throw std::runtime_error("DIRECT fixture buffer result mismatch");
    };
    check(s.A.Get(),4,3); check(s.B.Get(),4,3); check(s.Readback.Get(),8,7);
    void* data=nullptr; D3D12_RANGE range{0,4}; ThrowIfFailed(s.Pixel->Map(0,&range,&data));
    const auto* pixel=static_cast<const uint8_t*>(data);
    const bool valid=pixel[0]==s.Seed*8+7 && pixel[1]==0 && pixel[2]==255 && pixel[3]==255;
    const auto actual=std::to_string(pixel[0])+","+std::to_string(pixel[1])+","+std::to_string(pixel[2])+","+std::to_string(pixel[3]);
    D3D12_RANGE written{0,0}; s.Pixel->Unmap(0,&written);
    if(!valid) throw std::runtime_error("DIRECT fixture draw pixel mismatch: actual="+actual+" expected R="+std::to_string(s.Seed*8+7));
}
}

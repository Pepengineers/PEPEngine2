#include "Engine.RendererDX12/RenderPasses/GDX12ReflectionLightingPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12ReflectionProbePass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12ReflectionFallbackPass.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace
{
    struct ProbeRecord { XMFLOAT3 Position; UINT CubeIndex; XMFLOAT3 Extents; UINT MipCount; };
    static_assert(sizeof(ProbeRecord) == 32);
    void CreateFallback(GDX12DeviceResources& resources, GDX12Texture& output, GDX12Texture& depth)
    {
        GDX12TextureDesc desc;
        desc.Width=depth.GetWidth(); desc.Height=depth.GetHeight();
        desc.Format=desc.SRVDesc.Format=desc.UAVDesc.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.CreateUAV=true; desc.SRV_UAV_Heap=resources.SRV_UAV_Heap.get();
        desc.SRVHeapIndex=output.IsInitialized()?output.GetSRV()->HeapIndex:desc.SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex,TextureResources_RangeLength);
        desc.UAVHeapIndex=output.IsInitialized()?output.GetUAV()->HeapIndex:desc.SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex,TextureResources_RangeLength);
        desc.SRVDesc.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.SRVDesc.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D; desc.SRVDesc.Texture2D.MipLevels=1;
        desc.UAVDesc.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D; output.Initialize(desc);
    }
}
struct GDX12ReflectionLightingPass::Data
{
    GDX12ReflectionProbePass* Source=nullptr;
    std::vector<GDX12PublishedReflectionProbe> Submitted;
    std::vector<std::unique_ptr<GDX12UploadBuffer<ProbeRecord>>> Records;
    std::unique_ptr<GDX12UploadBuffer<XMUINT4>> Globals;
    UINT Capacity=0;
    bool Active=false;
};
GDX12ReflectionLightingPass::GDX12ReflectionLightingPass(GDX12ReflectionProbePass* source):_data(std::make_unique<Data>()) { _data->Source=source; }
GDX12ReflectionLightingPass::~GDX12ReflectionLightingPass()=default;
void GDX12ReflectionLightingPass::Setup(GDX12DeviceResources* resources,GDX12DeviceResources* other,RenderPipelineCommonData* common)
{
    GDX12RenderPass::Setup(resources,other,common);
    common->ReflectionLighting=this;
}
void GDX12ReflectionLightingPass::Initialize()
{
    auto& d=*_data;
    d.Globals=std::make_unique<GDX12UploadBuffer<XMUINT4>>(_resources->Device,static_cast<UINT>(_resources->FrameConstants.size()));
    for(UINT i=0;i<_resources->FrameConstants.size();++i)
    {
        auto record=std::make_unique<GDX12UploadBuffer<ProbeRecord>>(_resources->Device,1,EBufferType::Upload,false);
        record->CreateSRV(_resources->SRV_UAV_Heap.get(),_resources->SRV_UAV_Heap->GetAvailableIndex(ConstantsResources));
        record->CopyData(0,ProbeRecord{}); d.Records.push_back(std::move(record)); d.Globals->CopyData(i,XMUINT4{});
    }
    d.Capacity=1;
}
void GDX12ReflectionLightingPass::Submit(std::vector<GDX12PublishedReflectionProbe> probes)
{
    if(_data->Source) throw std::logic_error("A capture-backed probe uploader cannot accept another source");
    _data->Submitted=std::move(probes);
}
void GDX12ReflectionLightingPass::PrepareFrame()
{
    auto& d=*_data; d.Active=ReflectionProbeLightingEnabled();
    const size_t desired=d.Active?(d.Source?d.Source->GetSubmittedCount():d.Submitted.size()):0;
    if(desired>TextureCube_RangeLength) throw std::invalid_argument("Reflection probe count exceeds the cubemap descriptor range");
    if(desired>d.Capacity)
    {
        _resources->Device->GetCommandQueue()->Flush();
        for(auto& record:d.Records) record->Resize(static_cast<UINT>(desired));
        d.Capacity=static_cast<UINT>(desired);
    }
}
void GDX12ReflectionLightingPass::Execute(GDX12CommandList* commands)
{
    auto& d=*_data; const auto& probes=d.Source?d.Source->GetPublished():d.Submitted;
    const UINT count=d.Active?static_cast<UINT>(probes.size()):0,frame=_resources->CurrFrameConstantsIndex;
    if(count>d.Capacity) throw std::logic_error("Probe capacity must be prepared before recording frame commands");
    for(UINT i=0;i<count;++i)
    {
        const auto& probe=probes[i]; const auto& desc=probe.Description;
        if(!probe.Texture||!probe.Texture->IsInitialized()||!probe.Texture->HasSRV()||probe.MipCount==0||
            !std::isfinite(desc.Position.x)||!std::isfinite(desc.Position.y)||!std::isfinite(desc.Position.z)||
            !std::isfinite(desc.Extents.x)||!std::isfinite(desc.Extents.y)||!std::isfinite(desc.Extents.z)||
            desc.Extents.x<=0||desc.Extents.y<=0||desc.Extents.z<=0)
            throw std::invalid_argument("Invalid published reflection probe");
        const UINT slot=probe.Texture->GetSRV()->HeapIndex;
        const auto texture=probe.Texture->GetResource()->D3DResource->GetDesc();
        if(slot<TextureCube_StartIndex||slot>=TextureCube_StartIndex+TextureCube_RangeLength||
            texture.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||texture.DepthOrArraySize!=6||probe.MipCount>texture.MipLevels)
            throw std::invalid_argument("Published reflection probe requires a complete cubemap and its persistent cube SRV");
        d.Records[frame]->CopyData(i,ProbeRecord{desc.Position,slot-TextureCube_StartIndex,desc.Extents,probe.MipCount});
        commands->ResourceBarrier({probe.Texture->GetResource()->GetSRVBarrier()});
    }
    d.Globals->CopyData(frame,XMUINT4{count,0,0,0});
}
void GDX12ReflectionLightingPass::BindCompute(GDX12CommandList* commands,UINT globalsCB,UINT metadataSRV,UINT cubesTable)
{
    const UINT frame=_resources->CurrFrameConstantsIndex;
    commands->SetComputeRootConstantBufferView(globalsCB,_data->Globals->GetElementAddress(frame));
    commands->SetComputeSRV(metadataSRV,_data->Records[frame]->GetSRV()->GPUHandle);
    commands->SetComputeSRV(cubesTable,_resources->SRV_UAV_Heap->GetGPUHandle(TextureCube_StartIndex));
}
void GDX12ReflectionLightingPass::BindGraphics(GDX12CommandList* commands,UINT globalsCB,UINT metadataSRV,UINT cubesTable)
{
    const UINT frame=_resources->CurrFrameConstantsIndex;
    commands->SetGraphicsRootConstantBufferView(globalsCB,_data->Globals->GetElementAddress(frame));
    commands->SetGraphicsSRV(metadataSRV,_data->Records[frame]->GetSRV()->GPUHandle);
    commands->SetGraphicsSRV(cubesTable,_resources->SRV_UAV_Heap->GetGPUHandle(TextureCube_StartIndex));
}
void GDX12ReflectionLightingPass::ClearDenendencies()
{
    if(_commonData&&_commonData->ReflectionLighting==this) _commonData->ReflectionLighting=nullptr;
    GDX12RenderPass::ClearDenendencies(); _data.reset();
}

struct GDX12ReflectionFallbackPass::Data
{
    GDX12Texture Output;
    std::unique_ptr<GDX12RootSignature> Root;
    ComPtr<ID3D12PipelineState> Pipeline;
};
GDX12ReflectionFallbackPass::GDX12ReflectionFallbackPass():_data(std::make_unique<Data>())
{ _numInputs=4; _numOutputs=1; _flags=RENDER_PASS_FLAG_USE_CAMERAS; _outputs.push_back(&_data->Output); }
GDX12ReflectionFallbackPass::~GDX12ReflectionFallbackPass()=default;
void GDX12ReflectionFallbackPass::Initialize()
{
    if(_inputs.size()!=4) throw std::invalid_argument("Probe fallback requires depth, normal/roughness, global environment and baked mask");
    for(auto* input:_inputs) if(!input||!input->IsInitialized()) throw std::invalid_argument("Uninitialized probe fallback input");
    for(UINT i : {1u,3u})
        if(_inputs[0]->GetTexture()->GetWidth()!=_inputs[i]->GetTexture()->GetWidth()||_inputs[0]->GetTexture()->GetHeight()!=_inputs[i]->GetTexture()->GetHeight())
            throw std::invalid_argument("Probe fallback surface grids must match");
    auto& d=*_data; CreateFallback(*_resources,d.Output,*_inputs[0]->GetTexture());
    const bool probes=_commonData->ReflectionLighting!=nullptr;
    GDX12RootSignatureDesc root; root.NumSingleCBVSlots=probes?2:1; root.NumSingleSRVSlots=probes?5:4; root.NumSingleUAVSlots=1;
    if(probes) root.SRVRanges.emplace_back(TextureCube_RangeLength);
    root.StaticSamplers=GetStaticSamplers(); d.Root=std::make_unique<GDX12RootSignature>(_resources->Device,root);
    D3D_SHADER_MACRO macros[]={{"PROBE_LIGHTING","1"},{nullptr,nullptr}};
    auto shader=GDX12ShaderCompiler::GetInstance().CompileShader(_resources->Device,SHADERS_FOLDER "ReflectionFallback.hlsl",probes?macros:nullptr,"CS","cs");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline={};pipeline.pRootSignature=d.Root->GetRootSignature().Get();pipeline.CS={shader->GetBufferPointer(),shader->GetBufferSize()};
    ThrowIfFailed(_resources->Device->GetDevice()->CreateComputePipelineState(&pipeline,IID_PPV_ARGS(&d.Pipeline)));
}
void GDX12ReflectionFallbackPass::Execute(GDX12CommandList* commands)
{
    auto& d=*_data; const auto& frame=_resources->FrameConstants[_resources->CurrFrameConstantsIndex];
    commands->BeginPixEvent("Prepare probe SSR fallback",Colors::SteelBlue);
    commands->SetDescriptorHeaps({_resources->SRV_UAV_Heap.get()});commands->SetComputeRootSignature(d.Root.get());commands->SetPipelineState(d.Pipeline);
    commands->SetComputeRootConstantBufferView(0,frame->CameraCB->GetElementAddress(_commonData->ActiveCameraCBufferIndex));
    for(UINT i=0;i<4;++i){auto* texture=_inputs[i]->GetTexture();commands->ResourceBarrier({texture->GetResource()->GetSRVBarrier()});commands->SetComputeSRV(i,texture->GetSRV()->GPUHandle);}
    if(_commonData->ReflectionLighting) _commonData->ReflectionLighting->BindCompute(commands,1,4,5);
    commands->ResourceBarrier({d.Output.GetResource()->GetUnorderedAccessBarrier()});commands->SetComputeUAV(0,d.Output.GetUAV()->GPUHandle);
    commands->Dispatch((d.Output.GetWidth()+7)/8,(d.Output.GetHeight()+7)/8,1);commands->ResourceBarrier({d.Output.GetResource()->GetSRVBarrier()});commands->EndPixEvent();
}
void GDX12ReflectionFallbackPass::Resize(){CreateFallback(*_resources,_data->Output,*_inputs[0]->GetTexture());}
void GDX12ReflectionFallbackPass::ClearDenendencies(){GDX12RenderPass::ClearDenendencies();_data.reset();}

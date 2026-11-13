#include "MetaRenderPipeline.h"
#include "Engine.RendererDX12/GDX12DeviceResources.h"
#include "Engine.RendererDX12/GDX12CommandQueue.h"
#include "Engine.RendererDX12/GDX12SwapChain.h"
#include "Engine.RendererDX12/GDX12TextureResource.h"
#include "Engine.RendererDX12/GDX12Descriptor.h"
#include "Engine.Core/Logging/Logger.h"
void MetaRenderPipeline::Initialize(RenderPipelineContext& c) {
    _fixture=std::make_unique<Meta::DirectFixture>(*c.Primary.Device,static_cast<uint32_t>(c.Primary.FrameConstants.size()));
    _executor=std::make_unique<Meta::Executor>(*c.Primary.Device,static_cast<uint32_t>(c.Primary.FrameConstants.size()),64);
    Log::Info("MetaRenderGraph","U3 parallel DIRECT executor selected; legacy graph Execute disabled");
}
void MetaRenderPipeline::BuildGraph(GDX12RenderGraph&,const RenderPipelineGraphInputs&) {
    throw std::logic_error("Meta pipeline cannot build legacy graph");
}
void MetaRenderPipeline::RebuildExecution(RenderPipelineContext&) {
    _schedule=_fixture->Plan(D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_PRESENT);
}
uint64_t MetaRenderPipeline::ExecuteFrame(RenderPipelineContext& c,GDX12CommandList* prefix,
    const std::function<void(GDX12CommandList*)>& finish) {
    const auto slot=c.Primary.CurrFrameConstantsIndex;
    _executor->Begin(slot);
    _fixture->Validate(slot);
    _fixture->Prepare(slot,(_frame++%16)+1);
    auto* target=c.BackBuffer->GetCurrentBuffer();
    // Explicit plan returns the target to PRESENT. UI helpers run sequentially
    // in a separate suffix with this same boundary; no global tracker mutation
    // occurs inside any plan recording callback.
    if (target->GetResource()->GetCurrentState()!=D3D12_RESOURCE_STATE_PRESENT)
        throw std::logic_error("UI boundary requires a PRESENT target tracker");
    auto* suffix=c.Primary.Device->GetCommandQueue()->GetCommandList();
    finish(suffix);
    const auto point=_executor->Execute(_schedule,_fixture->Resources(slot,target->GetResource()->D3DResource.Get()),
        [&](Meta::Id op,uint32_t list,ID3D12GraphicsCommandList* cmd) {
            _fixture->Record(slot,op,list,cmd,target->GetResource()->D3DResource.Get(),target->GetRTV()->CPUHandle,target->GetWidth(),target->GetHeight());
        },prefix,suffix);
    return point;
}

#include "MetaRenderPipeline.h"
#include "Engine.RendererDX12/GDX12DeviceResources.h"
#include "Engine.RendererDX12/GDX12CommandQueue.h"
#include "Engine.RendererDX12/GDX12SwapChain.h"
#include "Engine.RendererDX12/GDX12TextureResource.h"
#include "Engine.RendererDX12/GDX12Descriptor.h"
#include "Engine.Core/Logging/Logger.h"
#include "Common/RuntimePaths.h"
#include "App.Base/Modules/RenderModule.h"
#include <fstream>
void MetaRenderPipeline::Initialize(RenderPipelineContext& c) {
    _geometry=std::make_unique<Meta::SceneGeometry>(c);
    _sceneFlags.SetFlag(RENDER_PASS_FLAG_USE_GEOMETRY,true);
    _sceneFlags.SetFlag(RENDER_PASS_FLAG_USE_MATERIALS,true);
    _bindings.PrimaryPasses={&_sceneFlags};
    _executor=std::make_unique<Meta::Executor>(*c.Primary.Device,static_cast<uint32_t>(c.Primary.FrameConstants.size()),512);
    Log::Info("MetaRenderGraph","U4 scene geometry DIRECT executor selected; legacy graph Execute disabled");
}
void MetaRenderPipeline::BuildGraph(GDX12RenderGraph&,const RenderPipelineGraphInputs&) {
    throw std::logic_error("Meta pipeline cannot build legacy graph");
}
void MetaRenderPipeline::RebuildExecution(RenderPipelineContext& c) {
    _executor->Drain();
    _geometry->Resize(c.Common.WindowWidth,c.Common.WindowHeight);
}
bool MetaRenderPipeline::PrepareFrame(RenderPipelineContext& c,GDX12CommandList*) {
    _geometry->PrepareGrass(c,_verify?1.f/60.f:c.Common.GameTimer->DeltaTime());
    c.PrepareScene().Complete();
    return false;
}
void MetaRenderPipeline::ValidateQueued() {
    _executor->Drain();
    for(UINT slot=0;slot<_geometry->SlotCount();++slot) {
        const auto stem=(Engine::Core::GetExecutableDirectory()/"Logs"/("queued-slot-"+std::to_string(slot))).string();
        if(_geometry->Readback(slot,stem).empty())throw std::runtime_error("Empty queued geometry result");
    }
    Log::Info("MetaRenderGraph","Queued geometry frames drained; three slot readbacks validated");
}
uint64_t MetaRenderPipeline::ExecuteFrame(RenderPipelineContext& c,GDX12CommandList* prefix,
    const std::function<void(GDX12CommandList*)>& finish) {
    const auto slot=c.Primary.CurrFrameConstantsIndex;
    _executor->Begin(slot);
    _geometry->Freeze(slot,c.Common.ActiveCameraCBufferIndex,c.Runtime.FreezeSkeletalMeshes());
    auto* target=c.BackBuffer->GetCurrentBuffer();
    _geometry->Select(_verify);
    _schedule=_geometry->Plan(slot,target->GetResource()->D3DResource.Get());
    // Explicit plan returns the target to PRESENT. UI helpers run sequentially
    // in a separate suffix with this same boundary; no global tracker mutation
    // occurs inside any plan recording callback.
    if (target->GetResource()->GetCurrentState()!=D3D12_RESOURCE_STATE_PRESENT)
        throw std::logic_error("UI boundary requires a PRESENT target tracker");
    auto* suffix=c.Primary.Device->GetCommandQueue()->GetCommandList();
    finish(suffix);
    if(_verify) {
        const auto stem=(Engine::Core::GetExecutableDirectory()/"Logs"/("geometry-"+std::to_string(_frame))).string();
        const auto bindings=_geometry->Resources(slot,target->GetResource()->D3DResource.Get());
        const auto record=[&](Meta::Id op,uint32_t list,ID3D12GraphicsCommandList* cmd) { _geometry->Record(slot,op,list,cmd,target->GetResource()->D3DResource.Get()); };
        _executor->Execute(_schedule,bindings,record,prefix,nullptr,true);
        _executor->Drain();
        const auto serial=_geometry->Readback(slot,stem+"-serial");
        _executor->Begin(slot);
        _geometry->Select(false);
        _schedule=_geometry->Plan(slot,target->GetResource()->D3DResource.Get());
        std::ofstream schedule(stem+"-schedule.txt");_schedule.Write(schedule);
        std::ofstream operations(stem+"-operations.txt");_geometry->WriteOperations(slot,operations);
        std::ofstream trace(stem+"-cpu.json");
        const auto completion=_executor->Execute(_schedule,_geometry->Resources(slot,target->GetResource()->D3DResource.Get()),record,nullptr,suffix,false,nullptr,&trace);
        _executor->Drain();
        const auto parallel=_geometry->Readback(slot,stem+"-parallel");
        if(serial!=parallel) throw std::runtime_error("Geometry serial/parallel buffers or images differ");
        Log::Info("MetaRenderGraph","Geometry serial/parallel exact comparison passed frame=",_frame," bytes=",serial.size());
        ++_frame; return completion;
    }
    const auto point=_executor->Execute(_schedule,_geometry->Resources(slot,target->GetResource()->D3DResource.Get()),
        [&](Meta::Id op,uint32_t list,ID3D12GraphicsCommandList* cmd) {
            _geometry->Record(slot,op,list,cmd,target->GetResource()->D3DResource.Get());
        },prefix,suffix);
    return point;
}

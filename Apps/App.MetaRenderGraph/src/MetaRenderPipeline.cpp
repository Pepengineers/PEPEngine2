#include "MetaRenderPipeline.h"
#include "Engine.RendererDX12/GDX12DeviceResources.h"
#include "Engine.RendererDX12/GDX12CommandQueue.h"
#include "Engine.RendererDX12/GDX12SwapChain.h"
#include "Engine.RendererDX12/GDX12TextureResource.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include "Engine.RendererDX12/GDX12Descriptor.h"
#include "Engine.Core/Logging/Logger.h"
#include "Common/RuntimePaths.h"
#include "App.Base/Modules/RenderModule.h"
#include "App.Base/Modules/SceneManagerModule.h"
#include "Engine.Core/BenchmarkEngine.h"
#include "Common/ConsoleVariables.h"
#include <fstream>
#include <exception>
namespace
{
AutoConsoleVariable<bool> MetaClouds(L"Meta.Clouds.Enabled", false, L"Select the local MetaRenderGraph cloud source.",
                                     EConsoleVariableFlags::Default);
AutoConsoleVariable<bool> MetaAsyncQueues(L"Meta.Async.Queues", true,
    L"Place explicit local work on COMPUTE/COPY; false retains the single-DIRECT reference.", EConsoleVariableFlags::Default);
}
namespace { AutoConsoleVariable<bool> MetaRecordSerial(L"Meta.Record.Serial",false,
    L"Record Meta native command lists serially on the owner.",EConsoleVariableFlags::Default); }
void RunEnvironmentGpuTests(GDX12DeviceResources &, RenderPipelineCommonData &);
void RunLocalWaterGpuTests(GDX12DeviceResources &, RenderPipelineCommonData &);
void RunLocalParticlesGpuTests(GDX12Device&,bool interactionOnly=false);
void CheckWaterParticleEvidence(const std::string&,const Matrix&,const Matrix&,unsigned);
void CheckWaterMotionEvidence(const std::string&,const Matrix&,const Matrix&,unsigned,bool);
std::vector<uint8_t> CaptureTechniqueImages(GDX12Device&,const Meta::Schedule&,
    const std::vector<ID3D12Resource*>&,const std::vector<GDX12Texture*>&,const std::string&);
void CaptureTechniqueBuffers(GDX12Device&,const Meta::Schedule&,const std::vector<ID3D12Resource*>&,
    const std::vector<std::pair<std::string,GDX12Resource*>>&,const std::string&);

void MetaRenderPipeline::Initialize(RenderPipelineContext &c)
{
    // Slot-local AO and paired scene owners exceed the legacy transient range.
    c.Primary.SRV_UAV_Heap->UseTransientDescriptorTail();
    _post = std::make_unique<Meta::ScenePostUI>(c);
    _geometry = std::make_unique<Meta::SceneGeometry>(c);
    _environment = std::make_unique<Meta::SceneEnvironment>(c.Primary, c.Common);
    _lighting = std::make_unique<Meta::SceneLighting>(c.Primary, c.Common);
    _waterParticles = std::make_unique<Meta::SceneWaterParticles>(c);
    _sceneFlags.SetFlag(RENDER_PASS_FLAG_USE_GEOMETRY, true);
    _sceneFlags.SetFlag(RENDER_PASS_FLAG_USE_MATERIALS, true);
    _bindings.PrimaryPasses = _post->Passes();
    _bindings.PrimaryPasses.push_back(&_sceneFlags);
    _bindings.Reconstruction = &_post->Reconstruction();
    _bindings.FrameGenerationInputs = true;
    _executor = std::make_unique<Meta::Executor>(*c.Primary.Device,
                                                 static_cast<uint32_t>(c.Primary.FrameConstants.size()), 1024);
    Log::Info("MetaRenderGraph", "Single-GPU queue packet executor selected; legacy graph Execute disabled");
}
void MetaRenderPipeline::BuildGraph(GDX12RenderGraph &, const RenderPipelineGraphInputs &)
{
    throw std::logic_error("Meta pipeline cannot build legacy graph");
}
void MetaRenderPipeline::RebuildExecution(RenderPipelineContext &c)
{
    _executor->Drain();
    _waterParticles->Resize();
    _geometry->Resize(c.Common.DownscaledWidth, c.Common.DownscaledHeight);
    _environment->Resize();
}
bool MetaRenderPipeline::PrepareFrame(RenderPipelineContext &c, GDX12CommandList *)
{
    c.Common.AOAfterDepthPrepass = GetAmbientOcclusionExecutionPoint() == 0 && GetAmbientOcclusionSettings().IsActive();
    _geometry->PrepareGrass(c, _verify ? 1.f / 60.f : c.Common.GameTimer->DeltaTime());
    c.PrepareScene().Complete();
    return false;
}
void MetaRenderPipeline::CaptureBenchmark(GDX12Device& device,const std::string& stem) {
    _executor->Drain();
    auto buffers=_waterParticles->DiagnosticBuffers(false);
    auto water=_waterParticles->WaterBuffers();buffers.insert(buffers.end(),water.begin(),water.end());
    CaptureTechniqueBuffers(device,_schedule,_geometry->PlannedResources(_lastSlot),buffers,stem);
    CaptureTechniqueImages(device,_schedule,_geometry->PlannedResources(_lastSlot),
        {_post->DiagnosticImages()[3],_waterParticles->Depth(),_geometry->Depth(_lastSlot)},stem);
}
void MetaRenderPipeline::ValidateQueued(GDX12Device& device,RenderPipelineCommonData& common)
{
    _executor->Drain();
    for (UINT slot = 0; slot < _geometry->SlotCount(); ++slot)
    {
        const auto stem =
            (Engine::Core::GetExecutableDirectory() / "Logs" / ("queued-slot-" + std::to_string(slot))).string();
        if (_lighting->Readback(slot, stem).empty() || _geometry->Readback(slot, stem).empty())
            throw std::runtime_error("Empty queued geometry result");
    }
    Log::Info("MetaRenderGraph", "Queued geometry frames drained; three slot readbacks validated");
    const auto stem=(Engine::Core::GetExecutableDirectory()/"Logs/queued-water").string();
    CaptureTechniqueImages(device,_schedule,_geometry->PlannedResources(_lastSlot),
        {_waterParticles->Depth(),_waterParticles->Motion(),_geometry->Depth(_lastSlot),_geometry->Depth(_lastSlot)},
        stem+"-parallel-water");
    CheckWaterMotionEvidence(stem,common.CameraViewToClip,common.CameraView,11,true);
}
uint64_t MetaRenderPipeline::ExecuteFrame(RenderPipelineContext &c, GDX12CommandList *prefix,
                                          const std::function<void(GDX12CommandList *)> &finish)
{
    struct ResetOnFailure
    {
        Meta::SceneWaterParticles& Techniques;
        int Exceptions = std::uncaught_exceptions();
        ~ResetOnFailure() { if (std::uncaught_exceptions() > Exceptions) Techniques.Reset(); }
    } resetOnFailure{*_waterParticles};
    const bool verify = _verify;
    const auto slot = c.Primary.CurrFrameConstantsIndex;
    _lastSlot=slot;
    _geometry->Freeze(slot, c.Common.ActiveCameraCBufferIndex, c.Runtime.FreezeSkeletalMeshes());
    if (verify && !_environmentTestsRun)
    {
        RunLocalWaterGpuTests(c.Primary, c.Common);
        RunLocalParticlesGpuTests(*c.Primary.Device);
        RunEnvironmentGpuTests(c.Primary, c.Common);
        _environment->RestoreViewsAfterBaseline();
        _environmentTestsRun = true;
    }
    if (_cloudsEnabled != MetaClouds.GetValue())
    {
        _cloudsEnabled = MetaClouds.GetValue();
        GDX12CloudParameters parameters;
        parameters.Enabled = _cloudsEnabled;
        _environment->SetClouds(parameters);
    }
    const auto scenes = BenchmarkEngine::GetLocator().GetModule<SceneManagerModule>();
    const auto *world = scenes->GetWorld();
    const double delta = c.Common.GameTimer ? c.Common.GameTimer->DeltaTime() : 0;
    _environment->Freeze(world && !world->IsPaused() ? delta * world->GetTimeScale() : 0);
    _lighting->Freeze(*_geometry, slot);
    _waterParticles->Prepare(float(delta));
    auto *target = c.BackBuffer->GetCurrentBuffer();
    bool recordingSerial = verify;
    GDX12Texture* transparencyHDR=nullptr;
    GDX12Texture* beforePostHDR=nullptr;
    Meta::NativeWork validationWork;
    const auto append = [&](Meta::Graph &g, std::vector<ID3D12Resource *> &bindings) {
        validationWork.Commands.clear();
        _post->BeginPlan();
        _environment->Append(g, bindings);
        _lighting->SetHDRContinuation([&](auto& graph, auto& resources, auto* hdr, auto* probes) {
            auto* finalHDR = _waterParticles->Append(graph, resources, *_geometry, *_environment, hdr, probes,
                _lighting->ProbeRadiance(recordingSerial), recordingSerial);
            transparencyHDR=finalHDR;
            if(verify) {
                auto& copy=_validationHDR[recordingSerial?0:1];
                if(!copy) copy=std::make_unique<GDX12Texture>();
                if(!copy->IsInitialized() || copy->GetWidth()!=finalHDR->GetWidth() || copy->GetHeight()!=finalHDR->GetHeight())
                    GDX12RenderHelpers::InitializeComputeTexture(*copy,c.Primary,finalHDR->GetWidth(),finalHDR->GetHeight(),finalHDR->GetFormat());
                beforePostHDR=copy.get();
                auto* source=finalHDR->GetResource()->D3DResource.Get();
                auto* target=copy->GetResource()->D3DResource.Get();
                const auto src=Meta::NativeWork::ResourceFor(graph,resources,source,"HDR before post source");
                const auto dst=Meta::NativeWork::ResourceFor(graph,resources,target,"HDR before post snapshot");
                validationWork.Add(graph,"snapshot transparency HDR before post",{{src,Meta::Mode::Read,D3D12_RESOURCE_STATE_COPY_SOURCE},
                    {dst,Meta::Mode::Write,D3D12_RESOURCE_STATE_COPY_DEST}},[=](auto* cmd){cmd->CopyResource(target,source);});
            }
            // Diagnostic A/B still compares the existing geometry/lighting readbacks.
            // Stateful post/SDK and screen UI execute once on the displayed frame.
            if (!verify || !recordingSerial)
                _post->Append(graph, resources, finalHDR, _waterParticles->Depth(), _waterParticles->Motion(),
                    _waterParticles->ParticleMotion(), target, finish);
            return finalHDR;
        });
        _lighting->Append(g, bindings, *_geometry, *_environment, target, recordingSerial);
    };
    const auto earlyAO = [&](Meta::Graph &g, std::vector<ID3D12Resource *> &bindings) {
        _lighting->AppendAO(g, bindings);
    };
    const auto prelude = [&](Meta::Graph& graph, std::vector<ID3D12Resource*>& resources) {
        _post->AppendPosters(graph, resources,!verify || recordingSerial);
    };
    const auto reserve = [&] {
        uint64_t lists = 0;
        for (const auto &packet : _schedule.Packets)
            lists += uint64_t(packet.Lists) + 2;
        if (lists > UINT32_MAX)
            throw std::length_error("Native plan list capacity overflow");
        _executor->Reserve(uint32_t(lists));
    };
    _geometry->Select(verify);
    _schedule = _geometry->Plan(slot, target->GetResource()->D3DResource.Get(), append, earlyAO, prelude,
        verify || !MetaAsyncQueues.GetValue());
    reserve();
    if (verify)
    {
        const auto stem =
            (Engine::Core::GetExecutableDirectory() / "Logs" / ("geometry-" + std::to_string(_frame))).string();
        std::ofstream history(stem+"-history-inputs.txt");
        history<<"before serial "<<c.Common.ResetHistory<<' '<<c.Common.ResetUpscalerHistory<<' '<<c.Common.RequestHistoryReset<<'\n';
        const auto bindings = _geometry->PlannedResources(slot);
        const auto record = [&](Meta::Id op, uint32_t list, ID3D12GraphicsCommandList *cmd) {
            if (!validationWork.Record(op,cmd) && !_environment->Record(op, cmd) && !_lighting->Record(op, cmd) && !_waterParticles->Record(op, cmd) && !_post->Record(op, cmd))
                _geometry->Record(slot, op, list, cmd, target->GetResource()->D3DResource.Get());
        };
        const auto serialCompletion = _executor->Execute(_schedule, bindings, record, prefix, nullptr, true);
        _lighting->Submitted(true, serialCompletion);
        _executor->Drain();
        history<<"after serial "<<c.Common.ResetHistory<<' '<<c.Common.ResetUpscalerHistory<<' '<<c.Common.RequestHistoryReset<<'\n';
        CaptureTechniqueImages(*c.Primary.Device,_schedule,bindings,
            {_waterParticles->Depth(),_waterParticles->Motion(),transparencyHDR,_geometry->Depth(slot),beforePostHDR},stem+"-serial-water");
        CaptureTechniqueImages(*c.Primary.Device,_schedule,bindings,_waterParticles->DiagnosticImages(true),stem+"-serial-water-stages");
        CaptureTechniqueBuffers(*c.Primary.Device,_schedule,bindings,_waterParticles->DiagnosticBuffers(true),stem+"-serial-particles");
        auto serial = _geometry->Readback(slot, stem + "-serial");
        const auto serialLighting = _lighting->Readback(slot, stem + "-serial");
        serial.insert(serial.end(), serialLighting.begin(), serialLighting.end());
        _executor->Begin(slot);
        _geometry->Select(false);
        recordingSerial = false;
        _schedule = _geometry->Plan(slot, target->GetResource()->D3DResource.Get(), append, earlyAO, prelude,
            !MetaAsyncQueues.GetValue());
        reserve();
        std::ofstream schedule(stem + "-schedule.txt");
        _schedule.Write(schedule);
        std::ofstream operations(stem + "-operations.txt");
        _geometry->WriteOperations(slot, operations);
        std::ofstream trace(stem + "-cpu.json");
        const auto completion = _executor->Execute(_schedule, _geometry->PlannedResources(slot), record, nullptr,
                                                   nullptr, false, nullptr, &trace);
        _post->Submitted(slot);
        _environment->Submitted(completion);
        _lighting->Submitted(false, completion);
        _executor->Drain();
        history<<"after parallel "<<c.Common.ResetHistory<<' '<<c.Common.ResetUpscalerHistory<<' '<<c.Common.RequestHistoryReset<<'\n';
        CaptureTechniqueImages(*c.Primary.Device,_schedule,_geometry->PlannedResources(slot),
            {_waterParticles->Depth(),_waterParticles->Motion(),transparencyHDR,_geometry->Depth(slot),beforePostHDR},stem+"-parallel-water");
        CaptureTechniqueImages(*c.Primary.Device,_schedule,_geometry->PlannedResources(slot),_waterParticles->DiagnosticImages(false),stem+"-parallel-water-stages");
        CaptureTechniqueBuffers(*c.Primary.Device,_schedule,_geometry->PlannedResources(slot),_waterParticles->DiagnosticBuffers(false),stem+"-parallel-particles");
        if(_verifyPost) {
            auto images=_post->DiagnosticImages();images.push_back(target);
            CaptureTechniqueImages(*c.Primary.Device,_schedule,_geometry->PlannedResources(slot),images,stem+"-post");
        }
        auto parallel = _geometry->Readback(slot, stem + "-parallel");
        const auto parallelLighting = _lighting->Readback(slot, stem + "-parallel");
        parallel.insert(parallel.end(), parallelLighting.begin(), parallelLighting.end());
        if (serial != parallel)
            throw std::runtime_error("Geometry serial/parallel buffers or images differ");
        CheckWaterParticleEvidence(stem,c.Common.CameraViewToClip,c.Common.CameraView,_frame);
        Log::Info("MetaRenderGraph", "Geometry serial/parallel exact comparison passed frame=", _frame,
                  " bytes=", serial.size());
        ++_frame;
        return completion;
    }
    std::ofstream trace;
    if(_traceNextFrame) trace.open(Engine::Core::GetExecutableDirectory()/"Logs/benchmark-recording.json");
    _traceNextFrame=false;
    const auto point = _executor->Execute(
        _schedule, _geometry->PlannedResources(slot),
        [&](Meta::Id op, uint32_t list, ID3D12GraphicsCommandList *cmd) {
            if (!_environment->Record(op, cmd) && !_lighting->Record(op, cmd) && !_waterParticles->Record(op, cmd) && !_post->Record(op, cmd))
                _geometry->Record(slot, op, list, cmd, target->GetResource()->D3DResource.Get());
        },
        prefix, nullptr,MetaRecordSerial.GetValue(),nullptr,trace.is_open()?&trace:nullptr);
    _post->Submitted(slot);
    _environment->Submitted(point);
    _lighting->Submitted(false, point);
    return point;
}

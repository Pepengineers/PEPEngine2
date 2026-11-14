#include "MetaRenderPipeline.h"
#include "Engine.RendererDX12/GDX12DeviceResources.h"
#include "Engine.RendererDX12/GDX12CommandQueue.h"
#include "Engine.RendererDX12/GDX12SwapChain.h"
#include "Engine.RendererDX12/GDX12TextureResource.h"
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
AutoConsoleVariable<bool> MetaCrossAdapter(L"Meta.CrossAdapter.Enabled", false,
    L"Startup opt-in: deterministic independent two-adapter buffer producer (Meta app only).", EConsoleVariableFlags::Default);
AutoConsoleVariable<int> MetaSecondaryAdapter(L"Meta.CrossAdapter.SecondaryAdapter", -1,
    L"Explicit DXGI EnumAdapters1 hardware index, distinct from actual primary LUID; no automatic fallback.", EConsoleVariableFlags::Default);
AutoConsoleVariable<int> MetaPublicationSelection(L"Meta.CrossAdapter.Selection", 1,
    L"0: StrictPublication (newest issued export); 1: LatestCompleted (retain ready display during import).", EConsoleVariableFlags::Default);
AutoConsoleVariable<int> MetaProducerSeed(L"Meta.CrossAdapter.Seed", 1,
    L"Deterministic producer seed, frozen independently of primary Present.", EConsoleVariableFlags::Default);
AutoConsoleVariable<bool> MetaWater(L"Meta.Water.Enabled", false,
    L"Startup opt-in: independent secondary water solver through the Meta executor.", EConsoleVariableFlags::Default);
AutoConsoleVariable<int> MetaWaterAdapter(L"Meta.Water.SecondaryAdapter", -1,
    L"Explicit secondary DXGI hardware index for Meta water; startup only.", EConsoleVariableFlags::Default);
AutoConsoleVariable<int> MetaWaterSelection(L"Meta.Water.Selection", 1,
    L"0: Strict newest issued water; 1: LatestCompleted, retain ready water during import.", EConsoleVariableFlags::Default);
Meta::PublicationSelection WaterSelection()
{
    const int selection = MetaWaterSelection.GetValue();
    if (selection != 0 && selection != 1) throw std::invalid_argument("Meta.Water.Selection must be 0 or 1");
    return selection == 0 ? Meta::PublicationSelection::StrictPublication : Meta::PublicationSelection::LatestCompleted;
}
}
void RunEnvironmentGpuTests(GDX12DeviceResources &, RenderPipelineCommonData &);

void MetaRenderPipeline::Initialize(RenderPipelineContext &c)
{
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
    const auto frames=static_cast<uint32_t>(c.Primary.FrameConstants.size());
    if (MetaWater.GetValue()) {
        if (_verify || MetaCrossAdapter.GetValue())
            throw std::invalid_argument("Meta.Water requires ordinary Meta host and excludes the deterministic CrossAdapter fixture");
        WaterSelection();
        _waterTransfer = std::make_unique<Meta::WaterTransfer>(*c.Primary.Device, *c.Primary.SRV_UAV_Heap, MetaWaterAdapter.GetValue());
        _waterParticles->SetWaterTransfer(_waterTransfer.get(), WaterSelection);
        _executor = std::make_unique<Meta::Executor>(_waterTransfer->Submissions(), 0, frames, 1024);
    } else if(MetaCrossAdapter.GetValue()) {
        if(_verify) throw std::invalid_argument("CrossAdapter producer requires ordinary Meta host, not single-GPU verification mode");
        if(MetaPublicationSelection.GetValue()!=0 && MetaPublicationSelection.GetValue()!=1)
            throw std::invalid_argument("Meta.CrossAdapter.Selection must be 0 or 1");
        _crossAdapter=std::make_unique<Meta::CrossAdapterTransfer>(*c.Primary.Device,MetaSecondaryAdapter.GetValue(),frames);
        _crossAdapter->SetSeed(uint32_t(MetaProducerSeed.GetValue()));
        _executor=std::make_unique<Meta::Executor>(_crossAdapter->Submissions(),0,frames,1024);
        _crossAdapter->Start();
    } else _executor=std::make_unique<Meta::Executor>(*c.Primary.Device,frames,1024);
    Log::Info("MetaRenderGraph", "Queue packet executor selected; cross-adapter=",bool(_crossAdapter),"; water=",bool(_waterTransfer),"; legacy graph Execute disabled");
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
void MetaRenderPipeline::ValidateQueued()
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
    _geometry->Freeze(slot, c.Common.ActiveCameraCBufferIndex, c.Runtime.FreezeSkeletalMeshes());
    if (verify && !_environmentTestsRun)
    {
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
    const auto append = [&](Meta::Graph &g, std::vector<ID3D12Resource *> &bindings) {
        _post->BeginPlan();
        _environment->Append(g, bindings);
        _lighting->SetHDRContinuation([&](auto& graph, auto& resources, auto* hdr, auto* probes) {
            auto* finalHDR = _waterParticles->Append(graph, resources, *_geometry, *_environment, hdr, probes, recordingSerial);
            // Diagnostic A/B still compares the existing geometry/lighting readbacks.
            // Stateful post/SDK and screen UI execute once on the displayed frame.
            if (!verify || !recordingSerial)
                _post->Append(graph, resources, finalHDR, _waterParticles->Depth(), _waterParticles->Motion(),
                    _waterParticles->ParticleMotion(), target, finish);
            return finalHDR;
        });
        _lighting->Append(g, bindings, *_geometry, *_environment, target, recordingSerial);
        if(_crossAdapter) {
            const int selection=MetaPublicationSelection.GetValue();
            if(selection!=0 && selection!=1) throw std::invalid_argument("Meta.CrossAdapter.Selection must be 0 or 1");
            _crossAdapter->SetSeed(uint32_t(MetaProducerSeed.GetValue()));
            _crossAdapter->Append(g,bindings,slot,selection==0 ? Meta::PublicationSelection::StrictPublication:
                Meta::PublicationSelection::LatestCompleted);
        }
    };
    const auto earlyAO = [&](Meta::Graph &g, std::vector<ID3D12Resource *> &bindings) {
        _lighting->AppendAO(g, bindings);
    };
    const auto prelude = [&](Meta::Graph& graph, std::vector<ID3D12Resource*>& resources) {
        _post->AppendPosters(graph, resources);
    };
    const auto reserve = [&] {
        uint64_t lists = 0;
        for (const auto &packet : _schedule.Packets)
            lists += uint64_t(packet.Lists) + 2;
        if (lists > UINT32_MAX)
            throw std::length_error("Native plan list capacity overflow");
        _executor->Reserve(uint32_t(lists));
    };
    const auto execute = [&](const std::vector<ID3D12Resource*>& bindings,const Meta::Executor::Record& record,
        GDX12CommandList* first,bool serial=false,std::ostream* trace=nullptr) {
        const auto waits = _waterTransfer ? _waterTransfer->Waits(_schedule) :
            _crossAdapter ? _crossAdapter->Waits(_schedule) : std::vector<Meta::PacketWait>{};
        try {
            const auto point=_executor->Execute(_schedule,bindings,record,first,nullptr,serial,nullptr,trace,waits);
            if(_crossAdapter) _crossAdapter->RetireFrame(_schedule,_executor->Completions(),true);
            if (_waterTransfer) _waterTransfer->RetireFrame(_schedule, _executor->Completions(), true);
            return point;
        } catch(...) {
            if(_crossAdapter) try { _crossAdapter->RetireFrame(_schedule,_executor->Completions(),false); } catch(...) {}
            if (_waterTransfer) try { _waterTransfer->RetireFrame(_schedule, _executor->Completions(), false); } catch (...) {}
            throw;
        }
    };
    _geometry->Select(verify);
    _schedule = _geometry->Plan(slot, target->GetResource()->D3DResource.Get(), append, earlyAO, prelude,
        verify || !MetaAsyncQueues.GetValue());
    reserve();
    if (verify)
    {
        const auto stem =
            (Engine::Core::GetExecutableDirectory() / "Logs" / ("geometry-" + std::to_string(_frame))).string();
        const auto bindings = _geometry->PlannedResources(slot);
        const auto record = [&](Meta::Id op, uint32_t list, ID3D12GraphicsCommandList *cmd) {
            if (!_environment->Record(op, cmd) && !_lighting->Record(op, cmd) && !_waterParticles->Record(op, cmd) && !_post->Record(op, cmd))
                _geometry->Record(slot, op, list, cmd, target->GetResource()->D3DResource.Get());
        };
        const auto serialCompletion = execute(bindings,record,prefix,true);
        _lighting->Submitted(true, serialCompletion);
        _executor->Drain();
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
        const auto completion = execute(_geometry->PlannedResources(slot),record,nullptr,false,&trace);
        _post->Submitted(slot);
        _environment->Submitted(completion);
        _lighting->Submitted(false, completion);
        _executor->Drain();
        auto parallel = _geometry->Readback(slot, stem + "-parallel");
        const auto parallelLighting = _lighting->Readback(slot, stem + "-parallel");
        parallel.insert(parallel.end(), parallelLighting.begin(), parallelLighting.end());
        if (serial != parallel)
            throw std::runtime_error("Geometry serial/parallel buffers or images differ");
        Log::Info("MetaRenderGraph", "Geometry serial/parallel exact comparison passed frame=", _frame,
                  " bytes=", serial.size());
        ++_frame;
        return completion;
    }
    const auto point = execute(
        _geometry->PlannedResources(slot),
        [&](Meta::Id op, uint32_t list, ID3D12GraphicsCommandList *cmd) {
            if (!(_crossAdapter && _crossAdapter->Record(op,cmd)) && !(_waterTransfer && _waterTransfer->Record(op,cmd)) &&
                !_environment->Record(op, cmd) && !_lighting->Record(op, cmd) && !_waterParticles->Record(op, cmd) && !_post->Record(op, cmd))
                _geometry->Record(slot, op, list, cmd, target->GetResource()->D3DResource.Get());
        },
        prefix);
    _post->Submitted(slot);
    _environment->Submitted(point);
    _lighting->Submitted(false, point);
    return point;
}

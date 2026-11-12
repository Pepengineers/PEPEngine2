#pragma once
#include "App.Base/Rendering/IRenderPipeline.h"
#include "App.Base/Rendering/CrossAdapterWork.h"
#include "App.Base/Rendering/RenderMGPURuntime.h"
#include "Engine.RendererDX12/GDX12SnapshotTransport.h"
#include "Engine.RendererDX12/RenderPasses/GDX12ParticlesPass.h"
#include <chrono>

struct ParticleMGPUControls
{
    bool Secondary = false;
    bool Exact = true;
    UINT UpdateEveryNFrames = 1;
    double MaxAgeMs = 0;
    bool Scheduled = false;
};
ParticleMGPUControls GetParticleMGPUControls();

// Captures each input step before batching; cadence never discards depth, interaction or emitter events.
class CrossAdapterParticles final
{
public:
    CrossAdapterParticles(GDX12DeviceResources& primary, GDX12Device& secondary,
        RenderMGPURuntime& runtime, GDX12ParticleSimulationPass& simulation, GDX12ParticlesPass& draw);
    ~CrossAdapterParticles();
    bool PrepareFrame(RenderPipelineCommonData& common);
    void Configure(GDX12RenderGraph& graph, RenderPipelineBindings& bindings, IRenderPassLink* depth);
    void ResetWorld();
    void SetInteractionVersion(GDX12Submission::TransferVersion version) { _interactionVersion = version; }
    struct Diagnostics
    {
        bool Requested = false, Secondary = false, Exact = true, Ready = false, OverAge = false;
        double Desired = 0, Issued = 0, Completed = 0, Displayed = 0, AgeMs = 0, Debt = 0;
        double SourceTime = 0, DisplayedSourceTime = 0;
        double WaitMs = 0;
        UINT64 InputBytes = 0, OutputBytes = 0;
        GDX12Submission::CompletionPoint Producer, Imported;
        std::string Fallback;
    };
    Diagnostics GetDiagnostics() const
    {
        auto result = _diagnostics;
        result.WaitMs = _work.WaitMilliseconds;
        if (result.Ready)
            result.AgeMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - _frameStarted).count();
        result.OverAge = result.AgeMs > _controls.MaxAgeMs;
        return result;
    }

private:
    GDX12Submission::TransferVersion _interactionVersion{};
    struct Input;
    using Channel = CrossAdapterWork::Channel;
    struct History;
    using View = GDX12Snapshot::PhysicalView;
    using Use = CrossAdapterWork::Use;
    std::optional<std::vector<GDX12RenderGraph::LegacyExternalView>> Boundary();
    std::optional<GDX12Submission::CompletionPoint> Submit(const GDX12RenderGraph::LegacyBoundary&,
        GDX12Submission::CompletionPoint prefix);
    void Advance();
    void MoveOwner(bool secondary);
    void Publish();
    void CaptureDisplayHistory();
    void ApplyDisplayHistory();

    GDX12DeviceResources& _primary;
    GDX12DeviceResources _secondary;
    RenderMGPURuntime& _runtime;
    CrossAdapterWork _work;
    GDX12ParticleSimulationPass& _simulation;
    GDX12ParticlesPass& _draw;
    RenderPipelineCommonData* _common = nullptr;
    ParticleMGPUControls _controls;
    bool _active = false, _ownsSecondary = false, _initialized = false, _publishPending = false;
    double _completedSourceTime = 0;
    std::chrono::steady_clock::time_point _frameStarted;
    std::vector<std::shared_ptr<Input>> _inputs;
    std::vector<std::shared_ptr<History>> _history;
    std::vector<GDX12ParticleEmitter> _requested;
    std::unique_ptr<Channel> _inputChannel, _outputChannel;
    Diagnostics _diagnostics;
};

#pragma once

#include "Engine.RendererDX12/RenderPasses/GDX12EnvironmentIBLPass.h"

class RenderMGPURuntime;

// Owns the opt-in procedural source and its publication state. The environment
// pass and primary device must outlive it.
class CrossAdapterEnvironment final : public GDX12RenderPass
{
public:
    enum class Route { Direct, LocalCompute, SecondaryCompute };
    struct Diagnostics
    {
        Route Requested = Route::Direct;
        Route Effective = Route::Direct;
        GDX12CloudSnapshotAge DisplayAge;
        uint64_t EpochResets = 0;
        uint64_t Submitted = 0;
        uint64_t DisplayedInvocation = 0;
        uint64_t Repeats = 0;
        uint64_t ExactDeferred = 0;
        uint64_t LatestSkipped = 0;
        uint64_t OverAgeCrossings = 0;
    };

    CrossAdapterEnvironment(GDX12EnvironmentIBLPass& environment, GDX12DeviceResources& primary,
        GDX12Device* secondary = nullptr, RenderMGPURuntime* runtime = nullptr);
    ~CrossAdapterEnvironment() override;
    GDX12CloudComponent& CloudSource();
    const Diagnostics& GetDiagnostics() const;

    // Once per simulation tick, with world pause and scale already applied.
    // Returns whether the wrapper currently needs a legacy replacement point.
    bool PrepareFrame(double worldDeltaSeconds);
    void RecordRenderGraph(GDX12RenderGraphBuilder& graph) override;
    void Execute(GDX12CommandList* commands) override;
    IRenderPassLink* GetOutput(UINT index) override;
    void Resize() override;
    void ClearDenendencies() override;

    std::optional<GDX12Submission::CompletionPoint> Submit(const GDX12RenderGraph::LegacyBoundary& boundary,
        GDX12Submission::CompletionPoint prefix);

private:
    struct State;
    GDX12EnvironmentIBLPass& _environment;
    std::unique_ptr<State> _state;
};

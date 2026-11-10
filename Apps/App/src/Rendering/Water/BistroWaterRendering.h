#pragma once
#include "App.Base/Modules/SceneManagerModule.h"
#include "Components/BistroWaterComponents.h"
#include "Rendering/Water/CrossAdapterWater.h"
#include "Engine.RendererDX12/RenderPasses/GDX12WBOITTransparencyPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12WBOITCompositionPass.h"

class BistroWaterRendering final
{
public:
    explicit BistroWaterRendering(std::shared_ptr<SceneManagerModule> scenes);
    ~BistroWaterRendering();
    std::vector<GDX12FluidInstance> CollectScene(float deltaSeconds);
    void PrepareFrame(std::vector<GDX12FluidInstance> instances, RenderMGPURuntime* runtime,
        GDX12DeviceResources& primary, GDX12Device* secondary, GDX12Submission::FrameSubmissionId frame,
        bool surfaceEligible);
    bool WantsExactBoundary() const { return _crossAdapter.WantsExactBoundary(); }
    std::optional<GDX12Submission::CompletionPoint> SubmitExact(GDX12Submission::CompletionPoint prefix);
    void RetireFrame(GDX12Submission::CompletionPoint tail) { _crossAdapter.RetireFrame(tail); }
    CrossAdapterWater::Diagnostics GetMGPUWaterDiagnostics() const { return _crossAdapter.GetDiagnostics(); }

    GDX12FluidSurfacePass Surface;
    GDX12FluidSimulationPass Simulation{ &Surface };
    GDX12FluidResolvePass Resolve{ &Surface };
    GDX12WBOITTransparencyPass Transparency{ GDX12WBOITTransparencyPass::DepthPartition::Far };
    GDX12WBOITCompositionPass Composition;

private:
    void Subscribe(World&);
    void Unsubscribe(World&);
    struct Subscription { World* Scene; ListenerHandle Created, Updated; };
    std::shared_ptr<SceneManagerModule> _scenes;
    ListenerHandle _created = 0, _destroyed = 0;
    std::vector<Subscription> _worlds;
    uint64_t _owner = 0;
    std::uint64_t _routeCandidate = 0;
    CrossAdapterWater _crossAdapter;
};

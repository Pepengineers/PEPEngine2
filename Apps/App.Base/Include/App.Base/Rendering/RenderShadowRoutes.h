#pragma once

#include "App.Base/Rendering/CrossAdapterShadowRaster.h"
#include "App.Base/Rendering/RenderShadowSceneCapture.h"
#include "Engine.RendererDX12/GDX12RenderGeometry.h"

class RenderModule;
class GDX12CascadedShadowPass;
class GDX12DeviceResources;
class GDX12LocalShadowPass;
struct RenderPipelineBindings;

class RenderShadowRoutes final
{
public:
    RenderShadowRoutes(GDX12DeviceResources& primary, GDX12Device& secondary, RenderMGPURuntime& runtime,
        GDX12CascadedShadowPass& sun, GDX12LocalShadowPass& local);
    ~RenderShadowRoutes();
    using LendView = std::function<CrossAdapterSceneReplica::ResourceView(
        CrossAdapterSceneReplica::ResourceView, D3D12_RESOURCE_STATES)>;
    struct ApplicationFrame
    {
        const IRenderGeometrySource* Source = nullptr;
        std::vector<GDX12RenderPrimitiveHandle> Primitives;
        std::vector<CrossAdapterSceneReplica::GrassBatch> CaptureBatches, PrimaryBatches;
        std::function<void(const CapturedShadowScene&, CrossAdapterShadowRaster::Input&,
            CrossAdapterShadowRaster::Input&, const LendView&)> Prepare;
    };
    using ResolveApplication = std::function<ApplicationFrame(CrossAdapterSceneReplica::FrameStamp,
        const std::vector<GDX12ShadowOutput>&, const GDX12ShadowSelection&)>;

    void PrepareFrame(RenderModule& render);
    void UpdateQuality();
    void SetApplication(ResolveApplication resolve) { _resolveApplication = std::move(resolve); }
    CrossAdapterSceneReplica::FrameStamp GetFrameStamp() const { return _stamp; }
    CrossAdapterSceneReplica::ResourceView IdentifyPrimaryView(std::shared_ptr<GDX12Resource> native);
    void Configure(GDX12RenderGraph& graph, GDX12RenderPass& node, GDX12ShadowAtlas atlas,
        RenderPipelineBindings& bindings);
    void RetireFrame(GDX12Submission::CompletionPoint tail);
    void Invalidate();
    const CrossAdapterShadowRaster::SubmissionTrace* GetTrace(GDX12ShadowAtlas atlas) const;
    struct UnitDiagnostics
    {
        GDX12LocalShadowKey Light;
        bool Sun = false, RequestedSecondary = false, EffectiveSecondary = false, Latest = false;
        UINT Interval = 1;
        double SourceTime = 0;
        CrossAdapterShadows::Statistics Transfer;
        double AgeMilliseconds() const { return SourceTime > 0 ? (ShadowSourceTimeSeconds() - SourceTime) * 1000 : -1; }
    };
    const std::vector<UnitDiagnostics>& GetDiagnostics() const { return _diagnostics; }

private:
    struct AtlasRoute;
    std::optional<std::vector<GDX12RenderGraph::LegacyExternalView>> PrepareBoundary(GDX12ShadowAtlas atlas);
    std::optional<GDX12Submission::CompletionPoint> Submit(GDX12ShadowAtlas atlas,
        const GDX12RenderGraph::LegacyBoundary& boundary, GDX12Submission::CompletionPoint prefix);
    GDX12Submission::CompletionPoint SubmitPolicy(GDX12ShadowAtlas atlas, CrossAdapterShadowRaster::Input primary,
        CrossAdapterShadowRaster::Input secondary, GDX12Submission::CompletionPoint prefix);
    void ClearUnits(GDX12ShadowAtlas atlas);

    GDX12DeviceResources& _primary;
    GDX12Device& _secondary;
    RenderMGPURuntime& _runtime;
    GDX12CascadedShadowPass& _sun;
    GDX12LocalShadowPass& _local;
    CrossAdapterShadows _transfer;
    CrossAdapterShadowScene _replica;
    std::array<std::unique_ptr<AtlasRoute>, 2> _routes;
    RenderModule* _render = nullptr;
    GDX12ShadowSelection _selection;
    GDX12ShadowQuality _quality;
    ResolveApplication _resolveApplication;
    CrossAdapterSceneReplica::FrameStamp _stamp;
    std::uint64_t _invocation = 0;
    std::uint64_t _policyFrame = 0;
    double _sourceTime = 0;
    bool _policyEnabled = false;
    std::vector<UnitDiagnostics> _diagnostics;
};

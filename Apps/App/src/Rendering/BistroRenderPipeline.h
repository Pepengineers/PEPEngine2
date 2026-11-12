#pragma once
#include "App.Base/Rendering/CommonRenderPasses.h"
#include "App.Base/Modules/SceneManagerModule.h"

class BistroGrassGeometry;
class GDX12CloudComponent;
class BistroWaterRendering;
class BistroUIPosters;

class BistroRenderPipeline final : public IRenderPipeline
{
public:
    explicit BistroRenderPipeline(std::shared_ptr<SceneManagerModule>);
    ~BistroRenderPipeline() override;
    static bool IsCollisionViewEnabled();
    bool AllowsFrameGeneration() const override { return !IsCollisionViewEnabled(); }
#ifdef _DEBUG
    void DrawDebugControls(const RenderModule&) override;
    static void ToggleCollisionView();
#endif
    void Initialize(RenderPipelineContext&) override;
    void ResetWorldHistory() override;
    std::pair<size_t, size_t> GetWorldProbeReadiness() const override;
    void PrepareMaterialBindings(RenderPipelineContext&) override;
    void UpdateFrameSettings(RenderPipelineContext&) override;
    bool PrepareFrame(RenderPipelineContext&, GDX12CommandList*) override;
    void BuildGraph(GDX12RenderGraph&, const RenderPipelineGraphInputs&) override;
    void Resize(RenderPipelineContext&) override;
    const RenderPipelineBindings& GetBindings() const override;

private:
    friend class BistroRenderGraphTests;

    void LinkInputs(const RenderPipelineGraphInputs&);
    std::vector<GDX12GeometryViewPass*> GeometryPasses();
    std::shared_ptr<SceneManagerModule> _scenes;
    std::unique_ptr<BistroGrassGeometry> _grass;
    bool _grassAvailable = true;
    std::unique_ptr<BistroWaterRendering> _water;
    std::unique_ptr<BistroUIPosters> _posters;
    bool _posterGraphChanged = false;
    std::unique_ptr<GDX12CloudComponent> _clouds;
    std::unique_ptr<CommonRenderPasses> _passes;
#ifdef _DEBUG
    std::unique_ptr<GDX12RenderPass> _collisionPass;
    bool _collisionViewActive = false;
#endif
    RenderPipelineCommonData* _common = nullptr;
    bool _earlyAO = false;
    GDX12DeviceResources* _resources = nullptr;
    std::vector<std::unique_ptr<GDX12RenderPass>> _geometryJobs;
};

#pragma once
#include "App.Base/Rendering/CommonRenderPasses.h"
#include "App.Base/Modules/SceneManagerModule.h"

class BistroGrassGeometry;

class BistroRenderPipeline final : public IRenderPipeline
{
public:
    explicit BistroRenderPipeline(std::shared_ptr<SceneManagerModule>);
    ~BistroRenderPipeline() override;
    void Initialize(RenderPipelineContext&) override;
    void UpdateFrameSettings(RenderPipelineContext&) override;
    bool PrepareFrame(RenderPipelineContext&, GDX12CommandList*) override;
    void BuildGraph(GDX12RenderGraph&, const RenderPipelineGraphInputs&) override;
    void Resize(RenderPipelineContext&) override;
    const RenderPipelineBindings& GetBindings() const override;

private:
    void LinkInputs(const RenderPipelineGraphInputs&);
    GDX12RenderGraph::Pass AddGeometry(GDX12RenderGraph&, GDX12GeometryViewPass&, GDX12RenderGraph::Pass prerequisite);
    std::vector<GDX12GeometryViewPass*> GeometryPasses();
    std::shared_ptr<SceneManagerModule> _scenes;
    std::unique_ptr<BistroGrassGeometry> _grass;
    std::unique_ptr<CommonRenderPasses> _passes;
    RenderPipelineCommonData* _common = nullptr;
    bool _earlyAO = false;
    GDX12DeviceResources* _resources = nullptr;
    std::vector<std::unique_ptr<GDX12RenderPass>> _geometryJobs;
    std::vector<std::unique_ptr<GDX12RenderPass>> _retiredGeometryJobs;
    UINT _graphPatchCount = 0;
    std::vector<UINT> _graphViewCapacities;
};

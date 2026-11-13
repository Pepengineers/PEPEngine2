#pragma once
#include "App.Base/Rendering/IRenderPipeline.h"
#include "Techniques/SceneGeometry.h"
#include "Engine.RendererDX12/RenderPasses/GDX12GPUCullingPass.h"
class MetaRenderPipeline final : public IRenderPipeline {
public:
    explicit MetaRenderPipeline(bool verify=false):_verify(verify) {}
    void SetVerification(bool verify) { _verify=verify; }
    void ValidateQueued();
    void Initialize(RenderPipelineContext&) override;
    bool AllowsFrameGeneration() const override { return false; }
    bool PrepareFrame(RenderPipelineContext&,GDX12CommandList*) override;
    void BuildGraph(GDX12RenderGraph&,const RenderPipelineGraphInputs&) override;
    void Resize(RenderPipelineContext&) override {}
    const RenderPipelineBindings& GetBindings() const override { return _bindings; }
    bool OwnsExecution() const override { return true; }
    bool OwnsScenePreparation() const override { return true; }
    void RebuildExecution(RenderPipelineContext&) override;
    uint64_t ExecuteFrame(RenderPipelineContext&,GDX12CommandList*,const std::function<void(GDX12CommandList*)>&) override;
    void WriteExecution(std::ostream& os) const override { _schedule.Write(os); }
private:
    RenderPipelineBindings _bindings;
    std::unique_ptr<Meta::SceneGeometry> _geometry;
    GDX12GPUCullingPass _sceneFlags;
    std::unique_ptr<Meta::Executor> _executor;
    Meta::Schedule _schedule;
    uint32_t _frame=0;
    bool _verify=false;
};

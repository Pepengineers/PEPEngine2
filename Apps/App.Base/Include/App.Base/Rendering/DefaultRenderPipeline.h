#pragma once
#include "App.Base/Rendering/CommonRenderPasses.h"

class DefaultRenderPipeline final : public IRenderPipeline
{
public:
    void Initialize(RenderPipelineContext&) override;
    void UpdateFrameSettings(RenderPipelineContext&) override;
    bool PrepareFrame(RenderPipelineContext&, GDX12CommandList*) override;
    void BuildGraph(GDX12RenderGraph&, const RenderPipelineGraphInputs&) override;
    void Resize(RenderPipelineContext&) override;
    const RenderPipelineBindings& GetBindings() const override;

private:
    void LinkInputs(const RenderPipelineGraphInputs&);
    std::unique_ptr<CommonRenderPasses> _passes;
    RenderPipelineCommonData* _common = nullptr;
    bool _earlyAO = false;
};

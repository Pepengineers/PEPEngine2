#pragma once
#include "App.Base/Rendering/IRenderPipeline.h"
#include "DirectFixture.h"
class MetaRenderPipeline final : public IRenderPipeline {
public:
    void Initialize(RenderPipelineContext&) override;
    bool AllowsFrameGeneration() const override { return false; }
    bool PrepareFrame(RenderPipelineContext&,GDX12CommandList*) override { return false; }
    void BuildGraph(GDX12RenderGraph&,const RenderPipelineGraphInputs&) override;
    void Resize(RenderPipelineContext&) override {}
    const RenderPipelineBindings& GetBindings() const override { return _bindings; }
    bool OwnsExecution() const override { return true; }
    void RebuildExecution(RenderPipelineContext&) override;
    uint64_t ExecuteFrame(RenderPipelineContext&,GDX12CommandList*,const std::function<void(GDX12CommandList*)>&) override;
    void WriteExecution(std::ostream& os) const override { _schedule.Write(os); }
private:
    RenderPipelineBindings _bindings;
    std::unique_ptr<Meta::DirectFixture> _fixture;
    std::unique_ptr<Meta::Executor> _executor;
    Meta::Schedule _schedule;
    uint32_t _frame=0;
};

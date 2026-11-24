#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

// Updates only empty-depth pixels; alias outputs preserve geometry color and motion vectors.
class GDX12SkyBoxPass final : public GDX12RenderPass
{
public:
    GDX12SkyBoxPass();
    void Describe(PassDescription& description) const override;
    void Initialize(PassInitContext& context) override;
    void Execute(PassExecutionContext& context) override;

private:
    std::unique_ptr<GDX12RootSignature> _root;
    ComPtr<ID3D12PipelineState> _pipeline;
};

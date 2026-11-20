#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

class GDX12DeferredLightingPass final : public GDX12RenderPass
{
public:
    GDX12DeferredLightingPass();
    void Describe(PassDescription& s) const override;
    void Initialize(PassInitContext& context) override;
    void Execute(PassExecutionContext& context) override;

private:
    std::unique_ptr<GDX12RootSignature> _root;
    ComPtr<ID3D12PipelineState> _pipeline;
};

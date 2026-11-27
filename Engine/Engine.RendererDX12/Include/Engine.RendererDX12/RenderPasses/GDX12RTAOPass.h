#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

// RayQuery ambient visibility with material alpha-test, distances use scene centimetres.
class GDX12RTAOPass final : public GDX12RenderPass
{
public:
    GDX12RTAOPass();
    void Describe(PassDescription& description) const override;
    void Initialize(PassInitContext& context) override;
    void Execute(PassExecutionContext& context) override;

private:
    std::unique_ptr<GDX12RootSignature> _rootSignature;
    ComPtr<ID3D12PipelineState> _occlusionPipeline;
    float _radius = 0;
    float _intensity = 0;
    float _bias = 0;
    float _fadeStart = 0;
    float _fadeEnd = 0;
    UINT _samples = 0;
};

#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

// Builds full-resolution ambient visibility from reverse-Z depth and world-space
// normals, then applies two depth/normal-aware blur dispatches.
// Radius, Bias and fade distances use scene centimetres; visibility 1 is unoccluded.
class GDX12SSAOPass final : public GDX12RenderPass
{
public:
    GDX12SSAOPass();
    void Describe(PassDescription& description) const override;
    void Initialize(PassInitContext& context) override;
    void OnResourcesRecreated(PassInitContext& context) override;
    std::vector<ExternalResourceUse> GetExternalResources(PassExecutionContext& context) override;
    void Execute(PassExecutionContext& context) override;

private:
    void CreateBlurTexture(PassInitContext& context);

    std::unique_ptr<GDX12RootSignature> _root;
    ComPtr<ID3D12PipelineState> _occlusionPipeline;
    ComPtr<ID3D12PipelineState> _blurPipeline;
    std::unique_ptr<GDX12Texture> _blurred;
    float _radius = 0;
    float _intensity = 0;
    float _bias = 0;
    float _sharpness = 0;
    float _fadeStart = 0;
    float _fadeEnd = 0;
    UINT _samples = 0;
};

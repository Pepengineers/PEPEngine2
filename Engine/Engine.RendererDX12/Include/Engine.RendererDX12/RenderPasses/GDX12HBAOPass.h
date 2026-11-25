#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

// NVIDIA gl_ssao classic HBAO adapted to reverse-Z depth and world-space normals.
// Radius and fade distances use centimetres.
// The R32_FLOAT output is 1 for unoccluded surfaces and 0 for fully occluded ones.
class GDX12HBAOPass final : public GDX12RenderPass
{
public:
    GDX12HBAOPass();
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
    UINT _directions = 0;
    UINT _steps = 0;
};

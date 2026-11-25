#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include <array>
#include <cassert>
#include <XeGTAO.h>

// Intel XeGTAO with depth prefiltering, analytic AO and edge-aware denoising.
// Radius and fade distances use scene centimetres. OutAmbientOcclusion is
// full-resolution R32_FLOAT visibility: 1 is unoccluded, 0 is fully occluded.
class GDX12XeGTAOPass final : public GDX12RenderPass
{
public:
    GDX12XeGTAOPass();
    void Describe(PassDescription& description) const override;
    void Initialize(PassInitContext& context) override;
    void Execute(PassExecutionContext& context) override;
    void OnResourcesRecreated(PassInitContext& context) override;
    std::vector<ExternalResourceUse> GetExternalResources(PassExecutionContext& context) override;

private:
    // Pads working textures to 16 pixels for the five-mip prefilter and two-pixel
    // denoiser. Existing descriptor slots are reused when the output size changes.
    void CreateWorkingTextures(PassInitContext& context);

    std::unique_ptr<GDX12RootSignature> _root;
    ComPtr<ID3D12PipelineState> _prefilterPipeline;
    ComPtr<ID3D12PipelineState> _occlusionPipeline;
    ComPtr<ID3D12PipelineState> _denoisePipeline;
    ComPtr<ID3D12PipelineState> _resolvePipeline;
    std::unique_ptr<GDX12Texture> _depth;
    std::array<GDX12Descriptor, XE_GTAO_DEPTH_MIP_LEVELS - 1> _depthMipUAVs;
    std::array<std::unique_ptr<GDX12Texture>, 2> _aoTerms;
    std::unique_ptr<GDX12Texture> _edges;
    XeGTAO::GTAOSettings _settings;
    float _intensity = 0;
    float _sharpness = 0;
    float _fadeStart = 0;
    float _fadeEnd = 0;
    bool _temporalNoise = false;
    UINT _noiseIndex = 0;
};

#pragma once

#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include <FidelityFX/host/ffx_sssr.h>

// AMD SSSR and its reflections denoiser replace specular IBL in the HDR scene.
// Each pass owns one device-local context and its history.
class GDX12FFXSSSRPass final : public GDX12RenderPass
{
public:
    GDX12FFXSSSRPass();
    ~GDX12FFXSSSRPass() override;
    void Describe(PassDescription& description) const override;
    void Initialize(PassInitContext& context) override;
    void OnResourcesRecreated(PassInitContext& context) override;
    std::vector<ExternalResourceUse> GetExternalResources(PassExecutionContext& context) override;
    void Execute(PassExecutionContext& context) override;

    // Discards SSSR history after a camera cut.
    void ResetHistory();

private:
    // FrameGraph waits for prior GPU use before resizing these resources.
    void CreateResources(PassInitContext& context);
    void CreateContext(PassInitContext& context);

    std::unique_ptr<GDX12RootSignature> _rootSignature;
    ComPtr<ID3D12PipelineState> _preparePipeline;
    ComPtr<ID3D12PipelineState> _compositionPipeline;
    std::unique_ptr<GDX12Texture> _roughness;
    std::unique_ptr<GDX12Texture> _motionVectors;
    std::unique_ptr<GDX12Texture> _radiance;
    std::unique_ptr<GDX12Texture> _fallback;
    std::unique_ptr<FfxSssrContext> _context;
    std::vector<uint64_t> _scratch;
    FfxSssrDispatchDescription _settings{};
    Matrix _previousViewProjection = Identity4x4();
    Matrix _previousProjection = Identity4x4();
    XMFLOAT2 _previousJitter{};
    UINT _historyCameraIndex = UINT_MAX;
    float _maxRoughness = 0.6f;
    float _roughnessFade = 0.1f;
    float _intensity = 1.0f;
};

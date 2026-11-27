#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include <NRD.h>

// RayQuery AO with alpha-test and NRD REBLUR history/filtering on this pass's GPU.
// Distances use scene centimetres.
class GDX12RTAOPass final : public GDX12RenderPass
{
public:
    GDX12RTAOPass();
    ~GDX12RTAOPass() override;
    void Describe(PassDescription& description) const override;
    void Initialize(PassInitContext& context) override;
    void OnResourcesRecreated(PassInitContext& context) override;
    std::vector<ExternalResourceUse> GetExternalResources(PassExecutionContext& context) override;
    void Execute(PassExecutionContext& context) override;

    // Discards NRD history after a camera cut.
    void ResetHistory();

private:
    void InitializeDenoiser(PassInitContext& context);
    void CreateDenoisingTextures(PassInitContext& context);
    void Denoise(PassExecutionContext& context);

    std::unique_ptr<GDX12RootSignature> _rootSignature;
    ComPtr<ID3D12PipelineState> _occlusionPipeline;
    ComPtr<ID3D12PipelineState> _resolvePipeline;
    nrd::Instance* _denoiser = nullptr;
    std::unique_ptr<GDX12RootSignature> _denoisingRootSignature;
    std::vector<ComPtr<ID3D12PipelineState>> _denoisingPipelines;
    std::vector<std::unique_ptr<GDX12Texture>> _permanentTextures;
    std::vector<std::unique_ptr<GDX12Texture>> _transientTextures;
    std::unique_ptr<GDX12Texture> _hitDistance;
    std::unique_ptr<GDX12Texture> _packedNormals;
    std::unique_ptr<GDX12Texture> _viewDepth;
    std::unique_ptr<GDX12Texture> _denoisedOcclusion;
    // Each frame slot owns a separate region until FrameGraph waits for its fence.
    std::unique_ptr<GDX12UploadBuffer<BYTE>> _denoisingConstants;
    nrd::CommonSettings _denoisingSettings{};
    UINT _historyCameraIndex = UINT_MAX;
    float _radius = 0;
    float _intensity = 0;
    float _bias = 0;
    float _fadeStart = 0;
    float _fadeEnd = 0;
    UINT _samples = 0;
};

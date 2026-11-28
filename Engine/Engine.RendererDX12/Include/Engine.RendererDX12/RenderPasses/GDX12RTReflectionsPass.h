#pragma once

#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include "Engine.RendererDX12/GDX12RayTracingPipelineState.h"
#include "Engine.RendererDX12/GDX12ShaderTable.h"
#include <NRD.h>

// Traces and denoises one reflection bounce, replacing specular IBL in the HDR scene.
// Distances use scene centimetres.
class GDX12RTReflectionsPass final : public GDX12RenderPass
{
public:
    GDX12RTReflectionsPass();
    ~GDX12RTReflectionsPass() override;
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
    std::unique_ptr<GDX12RayTracingPipelineState> _reflectionPipeline;
    std::unique_ptr<GDX12ShaderTable> _shaderTable;
    ComPtr<ID3D12PipelineState> _compositionPipeline;
    std::unique_ptr<GDX12Texture> _reflections;
    std::unique_ptr<GDX12Texture> _packedNormals;
    std::unique_ptr<GDX12Texture> _viewDepth;
    // REBLUR can write motion vectors, so the shared input stays read-only.
    std::unique_ptr<GDX12Texture> _motionVectors;
    std::unique_ptr<GDX12Texture> _denoisedReflections;
    nrd::Instance* _denoiser = nullptr;
    std::unique_ptr<GDX12RootSignature> _denoisingRootSignature;
    std::vector<ComPtr<ID3D12PipelineState>> _denoisingPipelines;
    std::vector<std::unique_ptr<GDX12Texture>> _permanentTextures;
    std::vector<std::unique_ptr<GDX12Texture>> _transientTextures;
    std::unique_ptr<GDX12UploadBuffer<BYTE>> _denoisingConstants;
    nrd::CommonSettings _denoisingSettings{};
    UINT _historyCameraIndex = UINT_MAX;
    float _maxDistance = 0;
    float _maxRoughness = 0;
    float _roughnessFade = 0;
    float _bias = 0;
    float _intensity = 0;
    UINT _samplesPerPixel = 0;
    bool _checkerboardTracing = true;
};

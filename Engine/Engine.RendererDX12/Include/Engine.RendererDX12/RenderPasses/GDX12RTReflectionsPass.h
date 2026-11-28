#pragma once

#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include "Engine.RendererDX12/GDX12RayTracingPipelineState.h"
#include "Engine.RendererDX12/GDX12ShaderTable.h"

// Traces one reflection bounce and replaces specular IBL in the input HDR scene.
// Distances use scene centimetres.
class GDX12RTReflectionsPass final : public GDX12RenderPass
{
public:
    GDX12RTReflectionsPass();
    void Describe(PassDescription& description) const override;
    void Initialize(PassInitContext& context) override;
    void OnResourcesRecreated(PassInitContext& context) override;
    std::vector<ExternalResourceUse> GetExternalResources(PassExecutionContext& context) override;
    void Execute(PassExecutionContext& context) override;

private:
    void CreateReflectionTexture(PassInitContext& context);

    std::unique_ptr<GDX12RootSignature> _rootSignature;
    std::unique_ptr<GDX12RayTracingPipelineState> _reflectionPipeline;
    std::unique_ptr<GDX12ShaderTable> _shaderTable;
    ComPtr<ID3D12PipelineState> _compositionPipeline;
    std::unique_ptr<GDX12Texture> _reflections;
    float _maxDistance = 0;
    float _maxRoughness = 0;
    float _roughnessFade = 0;
    float _bias = 0;
    float _intensity = 0;
    UINT _samplesPerPixel = 0;
    UINT _frameIndex = 0;
};

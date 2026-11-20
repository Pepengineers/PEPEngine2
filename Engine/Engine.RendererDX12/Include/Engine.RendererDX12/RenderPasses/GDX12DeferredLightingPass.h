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
    void InitializeEnvironmentMaps(GDX12DeviceResources& resources);
    void FilterEnvironmentMap(PassExecutionContext& context);

    static constexpr UINT SpecularMipCount = 8;
    bool _environmentMapsReady = false;
    std::unique_ptr<GDX12RootSignature> _filterRoot;
    ComPtr<ID3D12PipelineState> _irradiancePipeline;
    ComPtr<ID3D12PipelineState> _prefilterPipeline;
    ComPtr<ID3D12PipelineState> _brdfPipeline;
    std::array<GDX12Descriptor, SpecularMipCount - 1> _mipViews;

    GDX12Texture _irradianceMap;
    GDX12Texture _prefilteredEnvironmentMap;
    GDX12Texture _brdfLut;
    std::unique_ptr<GDX12RootSignature> _root;
    ComPtr<ID3D12PipelineState> _pipeline;
};

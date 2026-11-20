#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

class GDX12IBLPrecomputePass final : public GDX12RenderPass
{
public:
    void Describe(PassDescription& description) const override;
    void Initialize(PassInitContext& context) override;
    void Execute(PassExecutionContext& context) override;

private:
    static constexpr UINT SpecularMipCount = 8;
    std::unique_ptr<GDX12RootSignature> _filterRoot;
    ComPtr<ID3D12PipelineState> _irradiancePipeline;
    ComPtr<ID3D12PipelineState> _prefilterPipeline;
    ComPtr<ID3D12PipelineState> _brdfPipeline;
    std::array<GDX12Descriptor, SpecularMipCount - 1> _mipViews;
};

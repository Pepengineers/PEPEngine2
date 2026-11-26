#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

class GDX12FrustumCullingPass final : public GDX12RenderPass
{
public:
	static constexpr const char* OutOpaqueDrawStream = "OutOpaqueDrawStream";
	static constexpr const char* OutTransparentDrawStream = "OutTransparentDrawStream";

	GDX12FrustumCullingPass();
	void Describe(PassDescription& description) const override;
	void ResolveResourceRequirements(PassResourceRequirementsContext& context) override;
	void Initialize(PassInitContext& context) override;
	void Execute(PassExecutionContext& context) override;

private:
	ComPtr<ID3DBlob> _frustumCullingShader;
	std::unique_ptr<GDX12RootSignature> _frustumCullingRootSignature;
	ComPtr<ID3D12PipelineState> _frustumCullingPipeline;

	ComPtr<ID3DBlob> _clearDrawCountsShader;
	std::unique_ptr<GDX12RootSignature> _clearDrawCountsRootSignature;
	ComPtr<ID3D12PipelineState> _clearDrawCountsPipeline;
};

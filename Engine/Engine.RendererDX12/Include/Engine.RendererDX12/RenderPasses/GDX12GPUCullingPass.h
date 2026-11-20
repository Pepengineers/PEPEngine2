#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

class GDX12GPUCullingPass : public GDX12RenderPass
{
public:
	static constexpr const char* OutOpaqueDrawStream = "OutOpaqueDrawStream";
	static constexpr const char* OutTransparentDrawStream = "OutTransparentDrawStream";

	GDX12GPUCullingPass();
	void Describe(PassDescription& s) const override;
	void ResolveResourceRequirements(PassResourceRequirementsContext& context) override;
	void Initialize(PassInitContext& context) override;
	void Execute(PassExecutionContext& context) override;

private:
	ComPtr<ID3DBlob> _cullingCS;
	std::unique_ptr<GDX12RootSignature> _cullingRS;
	ComPtr<ID3D12PipelineState> _cullingPSO;

	ComPtr<ID3DBlob> _bufferClearCS;
	std::unique_ptr<GDX12RootSignature> _bufferClearRS;
	ComPtr<ID3D12PipelineState> _bufferClearPSO;
};

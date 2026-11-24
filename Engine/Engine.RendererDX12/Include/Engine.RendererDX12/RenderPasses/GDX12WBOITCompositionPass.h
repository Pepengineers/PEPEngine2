#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

class GDX12WBOITCompositionPass : public GDX12RenderPass
{
public:
	static constexpr const char* InOpaqueTexture = "InOpaqueTexture";
	static constexpr const char* InAccumulationTexture = "InAccumulationTexture";
	static constexpr const char* InRevealageTexture = "InRevealageTexture";
	static constexpr const char* OutColorTexture = "OutColorTexture";
	// Accumulated transparency coverage: 0 for opaque-only pixels, 1 for full coverage.
	static constexpr const char* OutTransparencyMask = "OutTransparencyMask";

	void Describe(PassDescription& s) const override;
	void Initialize(PassInitContext& context) override;
	void Execute(PassExecutionContext& context) override;

private:
	ComPtr<ID3DBlob> _compositionVS;
	ComPtr<ID3DBlob> _compositionPS;
	std::unique_ptr<GDX12RootSignature> _compositionRS;
	ComPtr<ID3D12PipelineState> _compositionPSO;

};

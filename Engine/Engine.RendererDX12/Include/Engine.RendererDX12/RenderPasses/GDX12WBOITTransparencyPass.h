#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

class GDX12WBOITTransparencyPass : public GDX12RenderPass
{
public:
	static constexpr const char* InDrawStream = "InDrawStream";
	static constexpr const char* InDepthBuffer = "InDepthBuffer";
	static constexpr const char* OutAccumulationTexture = "OutAccumulationTexture";
	static constexpr const char* OutRevealageTexture = "OutRevealageTexture";

	GDX12WBOITTransparencyPass();
	void Describe(PassDescription& s) const override;
	void Initialize(PassInitContext& context) override;
	void Execute(PassExecutionContext& context) override;

private:
	ComPtr<ID3DBlob> _transparencyVS;
	ComPtr<ID3DBlob> _transparencyPS;
	std::unique_ptr<GDX12RootSignature> _transparencyRS;
	ComPtr<ID3D12CommandSignature> _transparencyCS;
	ComPtr<ID3D12PipelineState> _transparencyPSO;

};

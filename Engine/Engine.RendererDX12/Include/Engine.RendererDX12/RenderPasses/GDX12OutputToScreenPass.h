#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

// Tone maps linear HDR into the SDR backbuffer. Its sRGB RTV handles display encoding.
class GDX12OutputToScreenPass : public GDX12RenderPass
{
public:
	static constexpr const char* InColorTexture = "InColorTexture";
	static constexpr const char* InBackBuffer = "InBackBuffer";
	static constexpr const char* OutPresentedBackBuffer = "OutPresentedBackBuffer";

	void Describe(PassDescription& s) const override;
	void Initialize(PassInitContext& context) override;
	void Execute(PassExecutionContext& context) override;

private:
	std::unique_ptr<GDX12RootSignature> _rootSignature;
	ComPtr<ID3D12PipelineState> _pipeline;
};

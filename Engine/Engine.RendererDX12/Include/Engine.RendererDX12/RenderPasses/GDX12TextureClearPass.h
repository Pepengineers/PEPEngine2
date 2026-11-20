#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

class GDX12TextureClearPass : public GDX12RenderPass
{
public:
	static constexpr const char* InTexture = "InTexture";
	static constexpr const char* InDepthBuffer = "InDepthBuffer";
	static constexpr const char* OutClearedTexture = "OutClearedTexture";
	static constexpr const char* OutClearedDepthBuffer = "OutClearedDepthBuffer";

	void Describe(PassDescription& s) const override;
	void Execute(PassExecutionContext& context) override;
};

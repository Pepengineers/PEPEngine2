#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

class GDX12OutputToScreenPass : public GDX12RenderPass
{
public:
	static constexpr const char* InColorTexture = "InColorTexture";
	static constexpr const char* InBackBuffer = "InBackBuffer";
	static constexpr const char* OutPresentedBackBuffer = "OutPresentedBackBuffer";

	void Describe(PassDescription& s) const override;
	void Execute(PassExecutionContext& context) override;
};

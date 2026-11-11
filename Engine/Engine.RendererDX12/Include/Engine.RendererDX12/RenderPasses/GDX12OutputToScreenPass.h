#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

class GDX12OutputToScreenPass : public GDX12RenderPass
{
public:
	static constexpr const char* InColorTexture = "InColorTexture";
	static constexpr const char* InBackBuffer = "InBackBuffer";
	static constexpr const char* OutPresentedBackBuffer = "OutPresentedBackBuffer";

	void Describe(PassDescription& s) const override
	{
		auto& color = s.AddInputTexture(InColorTexture);
		color.Formats = {DXGI_FORMAT_R8G8B8A8_UNORM};
		color.Usages = ResourceUsage::CopySource;
		color.AllowShared = true;
		auto& target = s.AddInputTexture(InBackBuffer);
		target.Formats = {DXGI_FORMAT_R8G8B8A8_UNORM};
		target.Access = DataAccess::WriteOnly;
		target.Usages = ResourceUsage::CopyDest;
		auto& presented = s.AddOutputTexture(OutPresentedBackBuffer);
		presented.AliasInput = InBackBuffer;
		presented.Usages = ResourceUsage::Present;
		s.RequireSameTextureSize({InColorTexture, InBackBuffer});
	}

	void Execute(PassExecutionContext& context) override
	{
		auto* cmdList = context.GetCommandList();
		auto* source = context.GetInputTexture(InColorTexture).GetTexture();
		auto* target = context.GetInputTexture(InBackBuffer).GetTexture();
		cmdList->BeginPixEvent("Output to Screen", Colors::MediumPurple);
		cmdList->CopyResource(target->GetResource()->D3DResource.Get(), source->GetResource()->D3DResource.Get());
		cmdList->EndPixEvent();
	}
};

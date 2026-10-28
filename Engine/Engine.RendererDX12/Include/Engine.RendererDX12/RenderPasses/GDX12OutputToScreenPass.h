#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

class GDX12OutputToScreenPass : public GDX12RenderPass
{
public:
	static constexpr const char* Color = "Color";
	static constexpr const char* Target = "Target";
	static constexpr const char* Presented = "Presented";

	void Describe(PassDescription& s) const override
	{
		auto& color = s.AddInputTexture(Color);
		color.Formats = {DXGI_FORMAT_R8G8B8A8_UNORM};
		color.Usages = ResourceUsage::CopySource;
		color.AllowShared = true;
		auto& target = s.AddInputTexture(Target);
		target.Formats = {DXGI_FORMAT_R8G8B8A8_UNORM};
		target.Access = DataAccess::WriteOnly;
		target.Usages = ResourceUsage::CopyDest;
		auto& presented = s.AddOutputTexture(Presented);
		presented.AliasInput = Target;
		presented.Usages = ResourceUsage::Present;
		s.RequireSameTextureSize({Color, Target});
	}

	void Execute(PassExecutionContext& context) override
	{
		auto* cmdList = context.GetCommandList();
		auto* source = context.GetInputTexture(Color).GetTexture();
		auto* target = context.GetInputTexture(Target).GetTexture();
		cmdList->BeginPixEvent("Copy to Screen", Colors::Aqua);
		cmdList->CopyResource(target->GetResource()->D3DResource.Get(), source->GetResource()->D3DResource.Get());
		cmdList->EndPixEvent();
	}
};

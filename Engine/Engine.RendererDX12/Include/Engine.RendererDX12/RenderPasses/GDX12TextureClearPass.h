#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

class GDX12TextureClearPass : public GDX12RenderPass
{
public:
	static constexpr const char* Target = "Target";
	static constexpr const char* DepthTarget = "DepthTarget";
	static constexpr const char* Cleared = "Cleared";
	static constexpr const char* DepthCleared = "DepthCleared";

	void Describe(PassDescription& s) const override
	{
		auto& target = s.AddInputTexture(Target);
		target.Formats = {DXGI_FORMAT_R8G8B8A8_UNORM};
		target.Access = DataAccess::WriteOnly;
		target.Usages = ResourceUsage::RenderTarget;
		auto& depthTarget = s.AddInputTexture(DepthTarget);
		depthTarget.Formats = {DXGI_FORMAT_D32_FLOAT};
		depthTarget.Access = DataAccess::WriteOnly;
		depthTarget.Usages = ResourceUsage::DepthWrite;
		auto& cleared = s.AddOutputTexture(Cleared);
		cleared.AliasInput = Target;
		cleared.Usages = ResourceUsage::RenderTarget;
		auto& depthCleared = s.AddOutputTexture(DepthCleared);
		depthCleared.AliasInput = DepthTarget;
		depthCleared.Usages = ResourceUsage::DepthWrite;
	}

	void Execute(PassExecutionContext& context) override
	{
		auto* cmdList = context.GetCommandList();
		auto* target = context.GetInputTexture(Target).GetTexture();
		auto* depth = context.GetInputTexture(DepthTarget).GetTexture();
		cmdList->BeginPixEvent("Texture Clear", Colors::ForestGreen);
		cmdList->SetRenderTargets({target}, depth);
		cmdList->ClearRenderTargetView(target);
		cmdList->ClearDepthStencilView(depth);
		cmdList->EndPixEvent();
	}
};

#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

class GDX12TextureClearPass : public GDX12RenderPass
{
public:
	static constexpr const char* InTexture = "InTexture";
	static constexpr const char* InDepthBuffer = "InDepthBuffer";
	static constexpr const char* OutClearedTexture = "OutClearedTexture";
	static constexpr const char* OutClearedDepthBuffer = "OutClearedDepthBuffer";

	void Describe(PassDescription& s) const override
	{
		auto& target = s.AddInputTexture(InTexture);
		target.Formats = {DXGI_FORMAT_R8G8B8A8_UNORM};
		target.Access = DataAccess::WriteOnly;
		target.Usages = ResourceUsage::RenderTarget;
		auto& depthTarget = s.AddInputTexture(InDepthBuffer);
		depthTarget.Formats = {DXGI_FORMAT_D32_FLOAT};
		depthTarget.Access = DataAccess::WriteOnly;
		depthTarget.Usages = ResourceUsage::DepthWrite;
		auto& cleared = s.AddOutputTexture(OutClearedTexture);
		cleared.AliasInput = InTexture;
		cleared.Usages = ResourceUsage::RenderTarget;
		auto& depthCleared = s.AddOutputTexture(OutClearedDepthBuffer);
		depthCleared.AliasInput = InDepthBuffer;
		depthCleared.Usages = ResourceUsage::DepthWrite;
	}

	void Execute(PassExecutionContext& context) override
	{
		auto* cmdList = context.GetCommandList();
		auto* target = context.GetInputTexture(InTexture).GetTexture();
		auto* depth = context.GetInputTexture(InDepthBuffer).GetTexture();
		cmdList->BeginPixEvent("Texture Clear", Colors::SlateGray);
		cmdList->SetRenderTargets({target}, depth);
		cmdList->ClearRenderTargetView(target);
		cmdList->ClearDepthStencilView(depth);
		cmdList->EndPixEvent();
	}
};

#include "Engine.RendererDX12/RenderPasses/GDX12TextureClearPass.h"

void GDX12TextureClearPass::Describe(PassDescription& s) const
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

void GDX12TextureClearPass::Execute(PassExecutionContext& context)
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

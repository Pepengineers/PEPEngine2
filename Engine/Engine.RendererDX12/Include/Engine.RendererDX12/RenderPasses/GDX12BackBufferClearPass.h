#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

class GDX12BackBufferClearPass : public GDX12RenderPass
{
public:
	GDX12BackBufferClearPass() { _flags = RENDER_PASS_FLAG_PRESENTING; }

	void Initialize(GDX12DeviceResources* resources)
	{
		_resources = resources;
	}

	void Execute(GDX12CommandList* cmdList, GDX12Texture* IN_OUT_currentBackBuffer, GDX12Texture* IN_OUT_depthStencil)
	{
		cmdList->BeginPixEvent("Clear Back Buffer", Colors::Aqua);
		cmdList->SetViewport(IN_OUT_currentBackBuffer->GetViewport());
		cmdList->SetScissorRect(IN_OUT_currentBackBuffer->GetScissorRect());
		cmdList->SetRenderTargets({ IN_OUT_currentBackBuffer }, IN_OUT_depthStencil);
		cmdList->ClearRenderTargetView(IN_OUT_currentBackBuffer);
		cmdList->ClearDepthStencilView(IN_OUT_depthStencil);
		cmdList->EndPixEvent();
	}
};
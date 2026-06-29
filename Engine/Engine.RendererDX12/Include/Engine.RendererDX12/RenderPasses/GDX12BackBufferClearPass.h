#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

class GDX12BackBufferClearPass : public GDX12RenderPass
{
public:
	GDX12BackBufferClearPass() : IN_OUT_currentBackBuffer(nullptr), IN_OUT_depthStencil(nullptr)
	{ _flags = RENDER_PASS_FLAG_PRESENTING; }

	void LinkDependancies(IRenderPassLink* IN_OUT_currentBackBuffer,
		IRenderPassLink* IN_OUT_depthStencil)
	{
		this->IN_OUT_currentBackBuffer = IN_OUT_currentBackBuffer;
		this->IN_OUT_depthStencil = IN_OUT_depthStencil;
	}

	void Initialize(GDX12DeviceResources* resources, UINT width, UINT height) override
	{
		_resources = resources;
	}

	void Execute(GDX12CommandList* cmdList) override
	{
		GDX12Texture* currentBackBuffer = IN_OUT_currentBackBuffer->GetTexture();
		GDX12Texture* depthStencil = IN_OUT_depthStencil->GetTexture();

		cmdList->BeginPixEvent("Clear Back Buffer", Colors::Aqua);
		cmdList->SetViewport(currentBackBuffer->GetViewport());
		cmdList->SetScissorRect(currentBackBuffer->GetScissorRect());
		cmdList->SetRenderTargets({ currentBackBuffer }, depthStencil);
		cmdList->ClearRenderTargetView(currentBackBuffer);
		cmdList->ClearDepthStencilView(depthStencil);
		cmdList->EndPixEvent();
	}

private:
	IRenderPassLink* IN_OUT_currentBackBuffer;
	IRenderPassLink* IN_OUT_depthStencil;
};
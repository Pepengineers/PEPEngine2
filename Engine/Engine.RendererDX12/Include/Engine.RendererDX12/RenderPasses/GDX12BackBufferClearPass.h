#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

class GDX12BackBufferClearPass : public GDX12RenderPass
{
public:
	GDX12BackBufferClearPass() : IN_OUT_currentBackBuffer(nullptr), IN_OUT_depthStencil(nullptr)
	{ _flags = RENDER_PASS_FLAG_NONE; }

	// Input 0 - back buffer
	// Input 1 - depth stencil
	// Output 0 - back buffer
	// Output 1 - depth stencil
	void LinkDependancies(std::vector<IRenderPassLink*> inputs, std::vector<IRenderPassLink*>* outputs) override
	{
		IN_OUT_currentBackBuffer = inputs[0];
		IN_OUT_depthStencil = inputs[1];

		outputs->push_back(IN_OUT_currentBackBuffer);
		outputs->push_back(IN_OUT_depthStencil);
	}

	void Execute(GDX12CommandList* cmdList) override
	{
		GDX12Texture* currentBackBuffer = IN_OUT_currentBackBuffer->GetTexture();
		GDX12Texture* depthStencil = IN_OUT_depthStencil->GetTexture();

		cmdList->BeginPixEvent("Clear Back Buffer", Colors::Aqua);
		cmdList->SetViewport(currentBackBuffer->GetViewport());
		cmdList->SetScissorRect(currentBackBuffer->GetScissorRect());
		cmdList->EnhancedTextureBarrier({ currentBackBuffer->GetResource()->GetRenderTargetEnhBarrier() });
		cmdList->ResourceBarrier({ depthStencil->GetResource()->GetDepthWriteBarrier() });
		cmdList->SetRenderTargets({ currentBackBuffer }, depthStencil);
		cmdList->ClearRenderTargetView(currentBackBuffer);
		cmdList->ClearDepthStencilView(depthStencil);
		cmdList->EndPixEvent();
	}

private:
	IRenderPassLink* IN_OUT_currentBackBuffer;
	IRenderPassLink* IN_OUT_depthStencil;
};
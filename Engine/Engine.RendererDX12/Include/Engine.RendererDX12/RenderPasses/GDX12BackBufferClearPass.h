#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

class GDX12BackBufferClearPass : public GDX12RenderPass
{
public:
	GDX12BackBufferClearPass() : IN_currentBackBuffer(nullptr), IN_depthStencil(nullptr)
	{  
		_flags = RENDER_PASS_FLAG_NONE; 
		_numInputs = 2;
		_numOutputs = 0;
	}

	// Input 0 - back buffer
	// Input 1 - depth stencil
	void Initialize() override
	{
		IN_currentBackBuffer = _inputs[0];
		IN_depthStencil = _inputs[1];
	}

	void Execute(GDX12CommandList* cmdList) override
	{
		GDX12Texture* currentBackBuffer = IN_currentBackBuffer->GetTexture();
		GDX12Texture* depthStencil = IN_depthStencil->GetTexture();

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

	void ClearDenendencies() override
	{
		GDX12RenderPass::ClearDenendencies();

		IN_currentBackBuffer = nullptr;
		IN_depthStencil = nullptr;
	}

private:
	IRenderPassLink* IN_currentBackBuffer;
	IRenderPassLink* IN_depthStencil;
};
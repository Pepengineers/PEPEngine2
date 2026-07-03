#pragma once

#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

class GDX12OutputToScreenPass : public GDX12RenderPass
{
public:
	GDX12OutputToScreenPass() : IN_Texture(nullptr), IN_OUT_BackBuffer(nullptr)
	{ _flags = RENDER_PASS_FLAG_NONE; }

	// Input 0 - InputTexture
	// Input 1 - BackBuffer
	void LinkDependancies(std::vector<IRenderPassLink*> inputs, std::vector<IRenderPassLink*>* outputs) override
	{
		IN_Texture = inputs[0];
		IN_OUT_BackBuffer = inputs[1];
	}

	void Execute(GDX12CommandList* cmdList)
	{
		GDX12Texture* inputTexture = IN_Texture->GetTexture();
		GDX12Texture* backBuffer = IN_OUT_BackBuffer->GetTexture();

		cmdList->BeginPixEvent("Copy To Screen Pass", Colors::Aqua);
		cmdList->ResourceBarrier({ inputTexture->GetResource()->GetCopySourceBarrier() });
		cmdList->EnhancedTextureBarrier({ backBuffer->GetResource()->GetCopyDestEnhBarrier() });

		cmdList->CopyResource(backBuffer->GetResource()->D3DResource.Get(),
			inputTexture->GetResource()->D3DResource.Get());
		cmdList->EndPixEvent();
	}

private:
	IRenderPassLink* IN_Texture;
	IRenderPassLink* IN_OUT_BackBuffer;
};
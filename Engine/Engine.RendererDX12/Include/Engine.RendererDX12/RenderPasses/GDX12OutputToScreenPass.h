#pragma once

#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

class GDX12OutputToScreenPass : public GDX12RenderPass
{
public:
	GDX12OutputToScreenPass() : IN_Texture(nullptr), IN_BackBuffer(nullptr)
	{  
		_flags = RENDER_PASS_FLAG_NONE; 
		_numInputs = 2;
		_numOutputs = 0;
	}

	// Input 0 - InputTexture
	// Input 1 - BackBuffer
	void Initialize() override
	{
		IN_Texture = _inputs[0];
		IN_BackBuffer = _inputs[1];
	}

	void Execute(GDX12CommandList* cmdList) override
	{
		GDX12Texture* inputTexture = IN_Texture->GetTexture();
		GDX12Texture* backBuffer = IN_BackBuffer->GetTexture();

		cmdList->BeginPixEvent("Copy To Screen Pass", Colors::Aqua);
		cmdList->ResourceBarrier({ inputTexture->GetResource()->GetCopySourceBarrier() });
		cmdList->ResourceBarrier({ backBuffer->GetResource()->GetCopyDestBarrier() });
		cmdList->CopyResource(backBuffer->GetResource()->D3DResource.Get(),
			inputTexture->GetResource()->D3DResource.Get());
		cmdList->ResourceBarrier({ backBuffer->GetResource()->GetPresentBarrier() });
		cmdList->EndPixEvent();
	}

	void ClearDenendencies() override
	{
		GDX12RenderPass::ClearDenendencies();

		IN_Texture = nullptr;
		IN_BackBuffer = nullptr;
	}

private:
	IRenderPassLink* IN_Texture;
	IRenderPassLink* IN_BackBuffer;
};
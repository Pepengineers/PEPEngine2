#pragma once

#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

class GDX12OutputToScreenPass : public GDX12RenderPass
{
public:
	GDX12OutputToScreenPass() : IN_Texture(nullptr), IN_OUT_BackBuffer(nullptr)
	{ _flags = RENDER_PASS_FLAG_NONE; }

	void LinkDependancies(IRenderPassLink* IN_Texture, IRenderPassLink* IN_OUT_BackBuffer)
	{
		this->IN_Texture = IN_Texture;
		this->IN_OUT_BackBuffer = IN_OUT_BackBuffer;
	}

	void Initialize(GDX12DeviceResources* resources, RenderPipelineCommonData* commonData) override
	{
		GDX12RenderPass::Initialize(resources, commonData);
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
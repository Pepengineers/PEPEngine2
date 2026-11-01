#pragma once
#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

class GDX12TextureClearPass : public GDX12RenderPass
{
public:
	GDX12TextureClearPass()
	{ _flags = RENDER_PASS_FLAG_NONE; }

	virtual void SetInputs(std::vector<IRenderPassLink*> inputs) override
	{
		_numOutputs = 0;
		_numInputs = static_cast<UINT>(inputs.size());
		GDX12RenderPass::SetInputs(std::move(inputs));
	}

	// Input N - TextureToClear
	void Initialize() override
	{
		IN_Textures.resize(_numInputs);
		for (UINT i = 0; i < _numInputs; i++) { IN_Textures[i] = _inputs[i]; }
	}

	// Clears all textures if they have either DSVs or RTVs
	void Execute(GDX12CommandList* cmdList) override
	{
		GDX12_SCOPED_EVENT(cmdList, "Texture clear pass", Colors::ForestGreen);
		for (UINT i = 0; i < _numInputs; i++)
		{
			GDX12Texture* inputTexture = IN_Textures[i]->GetTexture();

			if (inputTexture->HasRTV())
			{
				cmdList->ResourceBarrier({ inputTexture->GetResource()->GetRenderTargetBarrier() });
				cmdList->SetRenderTargets({ inputTexture }, nullptr);
				cmdList->ClearRenderTargetView(inputTexture);
				continue;
			}
			if (inputTexture->HasDSV())
			{
				cmdList->ResourceBarrier({ inputTexture->GetResource()->GetDepthWriteBarrier() });
				cmdList->SetRenderTargets({}, inputTexture);
				cmdList->ClearDepthStencilView(inputTexture);
				continue;
			}
		}
	}

	void ClearDenendencies() override
	{
		GDX12RenderPass::ClearDenendencies();
		IN_Textures.clear();
	}

private:
	std::vector<IRenderPassLink*> IN_Textures;
};
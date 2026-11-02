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
		_inputs = inputs;
		_numOutputs = 0;
		_numInputs = inputs.size();
	}

	// Input N - TextureToClear
	void Initialize() override
	{
		IN_Textures.resize(_numInputs);
		for (int i = 0; i < _numInputs; i++) { IN_Textures[i] = _inputs[i]; }
	}

	// Clears all textures if they have either DSVs or RTVs
	void Execute(GDX12CommandList* cmdList) override
	{
		cmdList->BeginPixEvent("Texture clear pass", Colors::ForestGreen);
		for (int i = 0; i < _numInputs; i++)
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
		cmdList->EndPixEvent();
	}

	void ClearDenendencies() override
	{
		GDX12RenderPass::ClearDenendencies();
		IN_Textures.clear();
	}

private:
	std::vector<IRenderPassLink*> IN_Textures;
};
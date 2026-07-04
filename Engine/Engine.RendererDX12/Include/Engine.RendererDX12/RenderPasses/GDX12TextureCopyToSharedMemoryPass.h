#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include "Engine.RendererDX12/GDX12SharedTexture.h"

class GDX12TextureCopyToSharedMemoryPass : public GDX12RenderPass
{
public:
	GDX12TextureCopyToSharedMemoryPass()
	{ _flags = RENDER_PASS_FLAG_NONE; }

	virtual void SetInputs(std::vector<IRenderPassLink*> inputs) override
	{
		_inputs = inputs;
		_numInputs = _numOutputs = inputs.size();

		OUT_SharedTextures.clear();
		_outputs.clear();
		OUT_SharedTextures.resize(_numOutputs);
		_outputs.resize(_numOutputs);
		for (int i = 0; i < _numOutputs; i++)
		{
			OUT_SharedTextures[i] = std::make_unique<GDX12SharedTexture>();
			_outputs[i] = OUT_SharedTextures[i].get();
		}
	}

	// Input N - InputTexture
	// Output N - OutputSharedTexture
	void Initialize() override
	{
		IN_Textures.resize(_numInputs);
		for (int i = 0; i < _numInputs; i++)
		{
			IN_Textures[i] = _inputs[i];
			OUT_SharedTextures[i]->Initialize(_resources->Device, _otherResources->Device,
				IN_Textures[i]->GetTexture()->GetWidth(), IN_Textures[i]->GetTexture()->GetHeight(),
				IN_Textures[i]->GetTexture()->GetFormat());
		}
	}

	// Copies textures from device the pass was initialized at to shared memory
	// Can be later used to read them on another device with CopyFromSharedMemoryPass
	void Execute(GDX12CommandList* cmdList) override
	{
		cmdList->BeginPixEvent("Texture Transfer To Shared Memory pass", Colors::ForestGreen);
		for (int i = 0; i < _numInputs; i++)
		{
			GDX12Texture* inTexture = IN_Textures[i]->GetTexture();
			GDX12SharedTexture* outSharedTexture = OUT_SharedTextures[i]->GetSharedTexture();

			cmdList->ResourceBarrier({ inTexture->GetResource()->GetCopySourceBarrier(),
		outSharedTexture->GetPrimaryResource()->GetCopyDestBarrier() });
			cmdList->CopyResource(outSharedTexture->GetPrimaryResource()->D3DResource.Get(),
				inTexture->GetResource()->D3DResource.Get());
			cmdList->ResourceBarrier({ outSharedTexture->GetPrimaryResource()->GetCommonBarrier() });
		}
		cmdList->EndPixEvent();
	}

	void Resize() override
	{
		UINT newWidth = GetFlagValue(RENDER_PASS_FLAG_USE_DOWNSCALED_RESOLUTION) ? _commonData->DownscaledWidth : _commonData->WindowWidth;
		UINT newHeight = GetFlagValue(RENDER_PASS_FLAG_USE_DOWNSCALED_RESOLUTION) ? _commonData->DownscaledHeight : _commonData->WindowHeight;
		for (auto& texture : OUT_SharedTextures) { texture->Resize(newWidth, newHeight); }
	}

	void ClearDenendencies() override
	{
		GDX12RenderPass::ClearDenendencies();
		IN_Textures.clear();
		OUT_SharedTextures.clear();
	}

private:
	std::vector<IRenderPassLink*> IN_Textures;
	std::vector<std::unique_ptr<GDX12SharedTexture>> OUT_SharedTextures;
};
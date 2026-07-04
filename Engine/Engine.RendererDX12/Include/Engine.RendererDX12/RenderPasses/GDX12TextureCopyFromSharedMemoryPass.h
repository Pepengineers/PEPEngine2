#pragma once
#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include "Engine.RendererDX12/GDX12SharedTexture.h"

class GDX12TextureCopyFromSharedMemoryPass : public GDX12RenderPass
{
public:
	GDX12TextureCopyFromSharedMemoryPass()
	{ _flags = RENDER_PASS_FLAG_NONE; }

	virtual void SetInputs(std::vector<IRenderPassLink*> inputs) override
	{
		_inputs = inputs;
		_numInputs = _numOutputs = inputs.size();

		OUT_Textures.clear();
		_outputs.clear();
		OUT_Textures.resize(_numOutputs);
		_outputs.resize(_numOutputs);
		for (int i = 0; i < _numInputs; i++)
		{
			OUT_Textures[i] = std::make_unique<GDX12Texture>();
			_outputs[i] = OUT_Textures[i].get();
		}
	}

	// Input N - SharedTexture
	// Output N - OutputTexture
	void Initialize() override
	{
		GDX12TextureDesc TextureDesc1;
		TextureDesc1.CreateSRV = true;
		TextureDesc1.SRV_UAV_Heap = _resources->SRV_UAV_Heap.get();
		TextureDesc1.SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		TextureDesc1.SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		TextureDesc1.SRVDesc.Texture2D.MipLevels = 1;
		TextureDesc1.SRVDesc.Texture2D.MostDetailedMip = 0;
		TextureDesc1.SRVDesc.Texture2D.PlaneSlice = 0;
		TextureDesc1.SRVDesc.Texture2D.ResourceMinLODClamp = 0.0f;

		IN_SharedTextures.resize(_numInputs);
		for (int i = 0; i < _numInputs; i++)
		{
			IN_SharedTextures[i] = _inputs[i];

			GDX12SharedTexture* sharedTexture = IN_SharedTextures[i]->GetSharedTexture();
			TextureDesc1.Format = TextureDesc1.RTVDesc.Format = TextureDesc1.SRVDesc.Format = sharedTexture->GetFormat();
			TextureDesc1.Width = sharedTexture->GetWidth();
			TextureDesc1.Height = sharedTexture->GetHeight();
			TextureDesc1.SRVHeapIndex = _resources->SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
			OUT_Textures[i]->Initialize(TextureDesc1);
		}
	}

	// Copies texture from shared memory onto device the pass was initialized on
	// It required CopyFromSharedMemory pass to be executed beforehand on the TransferFrom device
	// Resulting texture can be used as SRV on the device the texture was transferred to
	void Execute(GDX12CommandList* cmdList) override
	{
		cmdList->BeginPixEvent("Texture Transfer From Shared Memory pass", Colors::ForestGreen);
		for (int i = 0; i < _numInputs; i++)
		{
			GDX12SharedTexture* inSharedTexture = IN_SharedTextures[i]->GetSharedTexture();
			GDX12Texture* outTexture = OUT_Textures[i].get();

			cmdList->ResourceBarrier({ inSharedTexture->GetSecondaryResource()->GetCopySourceBarrier(),
			outTexture->GetResource()->GetCopyDestBarrier() });
			cmdList->CopyResource(outTexture->GetResource()->D3DResource.Get(),
				inSharedTexture->GetSecondaryResource()->D3DResource.Get());
			cmdList->ResourceBarrier({ inSharedTexture->GetSecondaryResource()->GetCommonBarrier() });
		}
		cmdList->EndPixEvent();
	}

	void Resize() override
	{
		UINT newWidth = GetFlagValue(RENDER_PASS_FLAG_USE_DOWNSCALED_RESOLUTION) ? _commonData->DownscaledWidth : _commonData->WindowWidth;
		UINT newHeight = GetFlagValue(RENDER_PASS_FLAG_USE_DOWNSCALED_RESOLUTION) ? _commonData->DownscaledHeight : _commonData->WindowHeight;
		for (auto& texture : OUT_Textures) { texture->Resize(newWidth, newHeight); }
	}

	void ClearDenendencies() override
	{
		GDX12RenderPass::ClearDenendencies();
		IN_SharedTextures.clear();
		OUT_Textures.clear();
	}

private:
	std::vector<IRenderPassLink*> IN_SharedTextures;
	std::vector<std::unique_ptr<GDX12Texture>> OUT_Textures;
};
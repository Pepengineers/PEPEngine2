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
		_numInputs = inputs.size();
		MatchOutputCount(_numInputs);
	}

	IRenderPassLink* GetOutput(UINT index) override
	{
		if (_inputs.empty()) { MatchOutputCount(index + 1); }
		return GDX12RenderPass::GetOutput(index);
	}

	// Input N - SharedTexture
	// Output N - OutputTexture
	void Initialize() override
	{
		IN_SharedTextures.resize(_numInputs);
		for (int i = 0; i < _numInputs; i++)
		{
			IN_SharedTextures[i] = _inputs[i];

			GDX12SharedTexture* sharedTexture = IN_SharedTextures[i]->GetSharedTexture();
			GDX12TextureDesc textureDesc;
			textureDesc.Format = sharedTexture->GetFormat();
			textureDesc.Width = sharedTexture->GetWidth();
			textureDesc.Height = sharedTexture->GetHeight();

			if (IsDepthFormat(textureDesc.Format))
			{
				textureDesc.CreateSRV = false;
				textureDesc.CreateDSV = true;
				textureDesc.DSVHeap = _resources->DSVHeap.get();
				textureDesc.DSVHeapIndex = _resources->DSVHeap->GetAvailableIndex();
				textureDesc.DSVDesc.Format = textureDesc.Format;
				textureDesc.DSVDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
				textureDesc.DSVDesc.Texture2D.MipSlice = 0;
			}
			else
			{
				textureDesc.CreateSRV = true;
				textureDesc.SRV_UAV_Heap = _resources->SRV_UAV_Heap.get();
				textureDesc.SRVHeapIndex = _resources->SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
				textureDesc.SRVDesc.Format = textureDesc.Format;
				textureDesc.SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
				textureDesc.SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
				textureDesc.SRVDesc.Texture2D.MipLevels = 1;
				textureDesc.SRVDesc.Texture2D.MostDetailedMip = 0;
				textureDesc.SRVDesc.Texture2D.PlaneSlice = 0;
				textureDesc.SRVDesc.Texture2D.ResourceMinLODClamp = 0.0f;
			}

			OUT_Textures[i]->Initialize(textureDesc);
		}
	}

	// Copies texture from shared memory onto device the pass was initialized on
	// It required CopyFromSharedMemory pass to be executed beforehand on the TransferFrom device
	// Resulting texture can be used as SRV or DSV on the device the texture was transferred to
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

	bool IsDepthFormat(DXGI_FORMAT format) const
	{
		switch (format)
		{
		case DXGI_FORMAT_D16_UNORM:
		case DXGI_FORMAT_D24_UNORM_S8_UINT:
		case DXGI_FORMAT_D32_FLOAT:
		case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
			return true;
		default:
			return false;
		}
	}

	void MatchOutputCount(UINT outputCount)
	{
		if (OUT_Textures.size() < outputCount) { OUT_Textures.resize(outputCount); }
		if (_outputs.size() < outputCount) { _outputs.resize(outputCount, nullptr); }

		for (UINT i = 0; i < outputCount; i++)
		{
			if (!OUT_Textures[i]) { OUT_Textures[i] = std::make_unique<GDX12Texture>(); }
			_outputs[i] = OUT_Textures[i].get();
		}

		_numOutputs = outputCount;
	}
};

#pragma once
#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include "Engine.RendererDX12/GDX12SharedTexture.h"

class GDX12TextureCopyFromSharedMemoryPass : public GDX12RenderPass
{
public:
	GDX12TextureCopyFromSharedMemoryPass() : IN_SharedTexture(nullptr), OUT_Texture(nullptr)
	{ _flags = RENDER_PASS_FLAG_NONE; }

	void LinkDependancies(IRenderPassLink* IN_SharedTexture,
		IRenderPassLink*& OUT_Texture)
	{
		GDX12SharedTexture* sharedTexture = IN_SharedTexture->GetSharedTexture();

		GDX12TextureDesc TextureDesc1;
		TextureDesc1.Format = TextureDesc1.RTVDesc.Format = TextureDesc1.SRVDesc.Format = sharedTexture->GetFormat();
		TextureDesc1.Width = sharedTexture->GetWidth();
		TextureDesc1.Height = sharedTexture->GetHeight();

		TextureDesc1.CreateSRV = true;
		TextureDesc1.SRV_UAV_Heap = _resources->SRV_UAV_Heap.get();
		TextureDesc1.SRVHeapIndex = _resources->SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
		TextureDesc1.SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		TextureDesc1.SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		TextureDesc1.SRVDesc.Texture2D.MipLevels = 1;
		TextureDesc1.SRVDesc.Texture2D.MostDetailedMip = 0;
		TextureDesc1.SRVDesc.Texture2D.PlaneSlice = 0;
		TextureDesc1.SRVDesc.Texture2D.ResourceMinLODClamp = 0.0f;

		this->OUT_Texture = std::make_unique<GDX12Texture>(TextureDesc1);

		OUT_Texture = this->OUT_Texture.get();
	}

	// Copies texture from shared memory onto device the pass was initialized on
	// It required CopyFromSharedMemory pass to be executed beforehand on the TransferFrom device
	// Resulting texture can be used as SRV on the device the texture was transferred to
	void Execute(GDX12CommandList* cmdList) override
	{
		GDX12SharedTexture* inTexture = IN_SharedTexture->GetSharedTexture();

		cmdList->BeginPixEvent("Texture Transfer From Shared Memory pass", Colors::ForestGreen);
		cmdList->ResourceBarrier({ inTexture->GetSecondaryResource()->GetCopySourceBarrier(),
		OUT_Texture->GetResource()->GetCopyDestBarrier() });
		cmdList->CopyResource(OUT_Texture->GetResource()->D3DResource.Get(),
			inTexture->GetSecondaryResource()->D3DResource.Get());
		cmdList->ResourceBarrier({ inTexture->GetSecondaryResource()->GetCommonBarrier() });
		cmdList->EndPixEvent();
	}

	void Resize() override
	{
		UINT newWidth = GetFlagValue(RENDER_PASS_FLAG_USE_DOWNSCALED_RESOLUTION) ? _commonData->DownscaledWidth : _commonData->WindowWidth;
		UINT newHeight = GetFlagValue(RENDER_PASS_FLAG_USE_DOWNSCALED_RESOLUTION) ? _commonData->DownscaledHeight : _commonData->WindowHeight;
		OUT_Texture->Resize(newWidth, newHeight);
	}

private:
	IRenderPassLink* IN_SharedTexture;
	std::unique_ptr<GDX12Texture> OUT_Texture;
};
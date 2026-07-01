#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include "Engine.RendererDX12/GDX12SharedTexture.h"

class GDX12TextureCopyToSharedMemoryPass : public GDX12RenderPass
{
public:
	GDX12TextureCopyToSharedMemoryPass() : IN_Texture(nullptr) , _transferToResources(nullptr)
	{ _flags = RENDER_PASS_FLAG_NONE; }

	void LinkDependancies(GDX12DeviceResources* transferToResources, IRenderPassLink* IN_Texture,
        IRenderPassLink*& OUT_SharedTexture)
	{
		_transferToResources = transferToResources;
		this->IN_Texture = IN_Texture;

		this->OUT_SharedTexture = std::make_unique<GDX12SharedTexture>();
		this->OUT_SharedTexture->Initialize(_resources->Device, _transferToResources->Device,
			IN_Texture->GetTexture()->GetWidth(), IN_Texture->GetTexture()->GetHeight(),
			IN_Texture->GetTexture()->GetFormat());

		OUT_SharedTexture = this->OUT_SharedTexture.get();
	}

	void Initialize(GDX12DeviceResources* resources, UINT width, UINT height) override
	{
		_resources = resources;
	}

	// Copies texture from device the pass was initialized at to shared memory
	// Can be later used to read it on another device with CopyFromSharedMemoryPass
	void Execute(GDX12CommandList* cmdList) override
	{
		GDX12Texture* inTexture = IN_Texture->GetTexture();

		cmdList->BeginPixEvent("Texture Transfer To Shared Memory pass", Colors::ForestGreen);
		cmdList->ResourceBarrier({ inTexture->GetResource()->GetCopySourceBarrier(),
		OUT_SharedTexture->GetPrimaryResource()->GetCopyDestBarrier() });
		cmdList->CopyResource(OUT_SharedTexture->GetPrimaryResource()->D3DResource.Get(), 
			inTexture->GetResource()->D3DResource.Get());
		cmdList->ResourceBarrier({ OUT_SharedTexture->GetPrimaryResource()->GetCommonBarrier() });
		cmdList->EndPixEvent();
	}

	void Resize(UINT width, UINT height) override
	{
		OUT_SharedTexture->Resize(width, height);
	}

private:
	GDX12DeviceResources* _transferToResources;
    IRenderPassLink* IN_Texture;
	std::unique_ptr<GDX12SharedTexture> OUT_SharedTexture;
};
#pragma once

#include "Engine.RendererDX12\D3DHelpers.h"

#include "Engine.Core/Types/TextureTypes.h"
#include "Engine.RendererDX12/IRenderPassLink.h"

class GDX12Descriptor;
class GDX12DescriptorHeap;
class GDX12Device;
class GDX12CommandList;
class GDX12TextureResource;

enum class ETextureSemantic
{
	Unknown,
	Color,
	Data,
};

namespace
{
	ETextureSemantic ConvertTextureSemantic(const Engine::Core::ETextureType textureType)
	{
		switch (textureType)
		{
		case Engine::Core::ETextureType::Color:
			return ETextureSemantic::Color;
		case Engine::Core::ETextureType::Data:
			return ETextureSemantic::Data;
		default:
			return ETextureSemantic::Unknown;
		}
	}
}

struct GDX12TextureDesc
{
	GDX12DescriptorHeap* SRV_UAV_Heap = nullptr;
	GDX12DescriptorHeap* RTVHeap = nullptr;
	GDX12DescriptorHeap* DSVHeap = nullptr;
	UINT Width = 0;
	UINT Height = 0;
	DXGI_FORMAT Format = DXGI_FORMAT_UNKNOWN;
	ETextureSemantic Semantic = ETextureSemantic::Unknown;
	bool CreateSRV = true;
	D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc = {};
	UINT SRVHeapIndex = -1;
	bool CreateRTV = false;
	D3D12_RENDER_TARGET_VIEW_DESC RTVDesc = {};
	UINT RTVHeapIndex = -1;
	bool CreateUAV = false;
	D3D12_UNORDERED_ACCESS_VIEW_DESC UAVDesc = {};
	UINT UAVHeapIndex = -1;
	bool CreateDSV = false;
	D3D12_DEPTH_STENCIL_VIEW_DESC DSVDesc = {};
	UINT DSVHeapIndex = -1;
	XMFLOAT4 ClearValue = { 0.f, 0.f, 0.f, 1.f };

	//If this is specified, it will be used as texture resource
	//It will not be wiped during resize, only views are recreated
	//Thus, resource management is now under user's control
	ComPtr<ID3D12Resource> ExternalResource = nullptr;
};

class GDX12Texture : public IRenderPassLink
{
public:
	GDX12Texture();
	~GDX12Texture();

	void Initialize(GDX12TextureDesc desc);

	//Recreates resources with new size and same descriptors
	//This will wipe all data on said resources, unless ExternalResource is provided
	void Resize(UINT width, UINT height);

	GDX12Descriptor* GetSRV();
	GDX12Descriptor* GetRTV();
	GDX12Descriptor* GetUAV();
	GDX12Descriptor* GetDSV();
	D3D12_CLEAR_VALUE& GetClearValue();
	GDX12TextureResource* GetResource();
	DXGI_FORMAT GetFormat();
	ETextureSemantic GetSemantic() const;
	D3D12_VIEWPORT GetViewport();
	D3D12_RECT GetScissorRect();
	UINT GetWidth();
	UINT GetHeight();
	GDX12Texture* GetTexture() override;
	bool IsInitialized() override;

private:
	void CreateResource();
	void CreateViews();

	std::unique_ptr<GDX12TextureResource> _resource;
	D3D12_RESOURCE_FLAGS _resourceFlags;
	GDX12Device* _device;
	GDX12TextureDesc _desc;
	std::unique_ptr<GDX12Descriptor> _srv;
	std::unique_ptr<GDX12Descriptor> _rtv;
	std::unique_ptr<GDX12Descriptor> _uav;
	std::unique_ptr<GDX12Descriptor> _dsv;
	D3D12_CLEAR_VALUE _clearValue;

	D3D12_VIEWPORT _viewport;
	D3D12_RECT _scissorRect;

	bool _isInitialized;
};
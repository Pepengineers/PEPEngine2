#pragma once

#include "Engine.RendererDX12\D3DHelpers.h"

class GDX12Descriptor;
class GDX12DescriptorHeap;
class GDX12Device;
class GDX12CommandList;

struct GDX12TextureDesc
{
	GDX12DescriptorHeap* SRV_UAV_Heap = nullptr;
	GDX12DescriptorHeap* RTVHeap = nullptr;
	GDX12DescriptorHeap* DSVHeap = nullptr;
	UINT Width = 0;
	UINT Height = 0;
	DXGI_FORMAT Format = DXGI_FORMAT_UNKNOWN;
	bool CreateSRV = true;
	D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc = {};
	bool CreateRTV = false;
	D3D12_RENDER_TARGET_VIEW_DESC RTVDesc = {};
	bool CreateUAV = false;
	D3D12_UNORDERED_ACCESS_VIEW_DESC UAVDesc = {};
	bool CreateDSV = false;
	D3D12_DEPTH_STENCIL_VIEW_DESC DSVDesc = {};
	XMFLOAT4 ClearValue = { 0.f, 0.f, 0.f, 1.f };

	//If this is specified, it will be used as texture resource
	//It will not be wiped during resize, only views are recreated
	//Thus, resource management is now under user's control
	ComPtr<ID3D12Resource> ExternalResource = nullptr;
};

class GDX12Texture
{
public:
	GDX12Texture(GDX12TextureDesc desc);
	~GDX12Texture();

	//Recreates resources with new size and same descriptors
	//This will wipe all data on said resources, unless ExternalResource is provided
	void Resize(UINT width, UINT height);

	GDX12Descriptor* GetSRV();
	GDX12Descriptor* GetRTV();
	GDX12Descriptor* GetUAV();
	GDX12Descriptor* GetDSV();
	D3D12_CLEAR_VALUE& GetClearValue();
	ComPtr<ID3D12Resource> GetD3DResource();
	DXGI_FORMAT GetFormat();


private:
	void CreateResource();
	void CreateViews();

	ComPtr<ID3D12Resource> _resource;
	D3D12_RESOURCE_FLAGS _resourceFlags;
	GDX12Device* _device;
	GDX12TextureDesc _desc;
	std::unique_ptr<GDX12Descriptor> _srv;
	std::unique_ptr<GDX12Descriptor> _rtv;
	std::unique_ptr<GDX12Descriptor> _uav;
	std::unique_ptr<GDX12Descriptor> _dsv;
	D3D12_CLEAR_VALUE _clearValue;
};
#pragma once

#include "Engine.RendererDX12\D3DHelpers.h"

#include "Engine.RendererDX12/GDX12ResourceHandle.h"

class GDX12CommandQueue;

//info on device capabilities
struct DeviceSpecs
{
	D3D_FEATURE_LEVEL MaxFeatureLevel;
	std::string Name;
	size_t DedicatedVideoMemory;  // in bytes
	size_t DedicatedSystemMemory; // in bytes
	size_t SharedSystemMemory;    // in bytes
	CD3DX12FeatureSupport Features;
};

enum EDeviceRole
{
	DEVICE_ROLE_PRIMARY,
	DEVICE_ROLE_SECONDARY
};

class GDX12Device : public std::enable_shared_from_this<GDX12Device>
{
public:
	GDX12Device();
	~GDX12Device();

	HRESULT Initialize(ComPtr<IDXGIAdapter4> adapter);
	void Reset();
	// Keep the returned handle alive until the GPU finishes; a raw resource reference does not retain its allocation.
	GDX12ResourceHandle CreateResource(D3D12_HEAP_TYPE heapType, const D3D12_RESOURCE_DESC& desc,
		D3D12_RESOURCE_STATES initialState, const D3D12_CLEAR_VALUE* clearValue = nullptr);
	D3D12MA::Allocator* GetMemoryAllocator() const { return _allocator.Get(); }

	const ComPtr<ID3D12Device14>& GetDevice();
	const ComPtr<ID3D12Device14>& GetCommandQueueCreationDevice();
	GDX12CommandQueue* GetCommandQueue();
	const DeviceSpecs& GetDeviceFeatures();
	LUID GetAdapterLuid() const;
	const bool IsInitialized();

	EDeviceRole Role;
private:
	void CollectDeviceFeatures();

	ComPtr<IDXGIAdapter4> _adapter;
	ComPtr<ID3D12Device14> _device;
	ComPtr<ID3D12Device14> _proxyDevice;
	ComPtr<D3D12MA::Allocator> _allocator;

	std::unique_ptr<GDX12CommandQueue> _commandQueue;

	bool _isInitialized;

	UINT _rtvDescriptorSize;
	UINT _dsvDescriptorSize;
	UINT _cbvSrvUavDescriptorSize;

	DeviceSpecs _specs;
};

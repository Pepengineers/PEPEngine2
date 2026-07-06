#pragma once

#include "Engine.RendererDX12\D3DHelpers.h"

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

	const ComPtr<ID3D12Device14>& GetDevice();
	const ComPtr<ID3D12Device14>& GetCommandQueueCreationDevice();
	GDX12CommandQueue* GetCommandQueue();
	const DeviceSpecs& GetDeviceFeatures();
	const bool IsInitialized();

	EDeviceRole Role;
private:
	void CollectDeviceFeatures();

	ComPtr<IDXGIAdapter4> _adapter;
	ComPtr<ID3D12Device14> _device;
	ComPtr<ID3D12Device14> _proxyDevice;

	std::unique_ptr<GDX12CommandQueue> _commandQueue;

	bool _isInitialized;

	UINT _rtvDescriptorSize;
	UINT _dsvDescriptorSize;
	UINT _cbvSrvUavDescriptorSize;

	DeviceSpecs _specs;
};

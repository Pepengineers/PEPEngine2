#pragma once

#include "Engine.RendererDX12\D3DHelpers.h"

class GDX12CommandQueue;

//info on device capabilities
struct DeviceSpecs
{
	D3D_FEATURE_LEVEL MaxFeatureLevel;
	D3D_SHADER_MODEL MaxShaderModel;
	bool RaytracingSupport;
	bool MeshShadersSupport;
	bool VariableRateShadingSupport;
	bool EnhancedBarriersSupport;
	std::string Name;
	size_t DedicatedVideoMemory;  // in bytes
	size_t DedicatedSystemMemory; // in bytes
	size_t SharedSystemMemory;    // in bytes
};

class GDX12Device : public std::enable_shared_from_this<GDX12Device>
{
public:
	GDX12Device();
	~GDX12Device();

	HRESULT Initialize(ComPtr<IDXGIAdapter4> adapter);
	void Reset();

	ComPtr<ID3D12Device14> GetDevice();
	std::shared_ptr<GDX12CommandQueue> GetCommandQueue();
	const DeviceSpecs& GetDeviceFeatures() const;
	const bool IsInitialized() const;

private:
	void CollectDeviceFeatures();

	ComPtr<IDXGIAdapter4> _adapter;
	ComPtr<ID3D12Device14> _device;

	std::shared_ptr<GDX12CommandQueue> _commandQueue;

	bool _isInitialized;

	UINT _rtvDescriptorSize;
	UINT _dsvDescriptorSize;
	UINT _cbvSrvUavDescriptorSize;

	DeviceSpecs _specs;
};
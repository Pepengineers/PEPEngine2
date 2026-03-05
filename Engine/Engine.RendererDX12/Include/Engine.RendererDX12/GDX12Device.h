#pragma once

#include "Engine.RendererDX12\D3DHelpers.h"

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

class GDX12Device
{
public:
	GDX12Device();
	~GDX12Device();

	HRESULT Initialize(IDXGIAdapter4* adapter = nullptr);
	void Reset();

	ComPtr<ID3D12Device5> GetDevice() const;
	const DeviceSpecs& GetDeviceFeatures() const;
	const bool IsInitialized() const;

private:
	ComPtr<ID3D12Device5> _device;
	bool _isInitialized;

	UINT _rtvDescriptorSize;
	UINT _dsvDescriptorSize;
	UINT _cbvSrvUavDescriptorSize;

	DeviceSpecs _specs;
};
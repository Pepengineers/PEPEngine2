#pragma once

#include "Engine.RendererDX12\D3DHelpers.h"

struct DeviceDesc
{
    ComPtr<IDXGIAdapter4> Adapter;
    std::wstring Name;
    size_t DedicatedVideoMemory;  // in bytes
    size_t DedicatedSystemMemory; // in bytes
    size_t SharedSystemMemory;    // in bytes

    DeviceDesc() :
        DedicatedVideoMemory(0),
        DedicatedSystemMemory(0),
        SharedSystemMemory(0)
        {}
};

class GDX12Device;

class GDX12DeviceFactory
{
public:
	static HRESULT Initialize();
	static const ComPtr<IDXGIFactory7>& GetFactory();
	static void Reset();

    // Gets all device adapters
    // In order from more to less performance
    static std::vector<DeviceDesc> GetDeviceDescriptors();
    static ComPtr<IDXGIAdapter4> GetDefaultAdapter();

    // Gets best adapter the system has
    // Basically does GetDeviceDescriptors[0]
    // Prefers dGPU over iGPUs but may confuse between 2 dGPUs
    static ComPtr<IDXGIAdapter4> GetMostPerformantAdapter();

    static ComPtr<IDXGISwapChain4> CreateSwapChain(GDX12Device* device, 
        DXGI_SWAP_CHAIN_DESC1& desc, HWND hwnd);

private:

	//forbid creating class instances
	GDX12DeviceFactory() = delete;

	static ComPtr<IDXGIFactory7> _dxgiFactory;
    static ComPtr<IDXGIFactory7> _dxgiFactoryProxy;
	static bool _isInitialized;
};
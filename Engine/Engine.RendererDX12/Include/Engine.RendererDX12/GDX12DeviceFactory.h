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

class GDX12DeviceFactory
{
public:
	static HRESULT Initialize();
	static ComPtr<IDXGIFactory7> GetFactory();
	static void Reset();

    // Gets all device adapters. First element is the default device
    static std::vector<DeviceDesc> GetDeviceDescriptors();
    static ComPtr<IDXGIAdapter4> GetDefaultAdapter();

private:

	//forbid creating class instances
	GDX12DeviceFactory() = delete;

	static ComPtr<IDXGIFactory7> _dxgiFactory;
	static bool _isInitialized;
};
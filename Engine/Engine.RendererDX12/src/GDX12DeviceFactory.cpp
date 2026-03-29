#include "Engine.RendererDX12/GDX12DeviceFactory.h"

#include "Engine.RendererDX12/GDX12Device.h"
#include "Engine.RendererDX12/GDX12CommandQueue.h"

ComPtr<IDXGIFactory7> GDX12DeviceFactory::_dxgiFactory = nullptr;
bool GDX12DeviceFactory::_isInitialized = false;

HRESULT GDX12DeviceFactory::Initialize()
{
    if (_isInitialized) { return S_OK; }

    HRESULT hr = CreateDXGIFactory2(D3D12_DEVICE_FACTORY_FLAG_ALLOW_RETURNING_EXISTING_DEVICE, IID_PPV_ARGS(&_dxgiFactory));

    if (SUCCEEDED(hr)) { _isInitialized = true; }
    return hr;
}

const ComPtr<IDXGIFactory7>& GDX12DeviceFactory::GetFactory()
{
    if (!_isInitialized) { ThrowIfFailed(Initialize()); }
    return _dxgiFactory;
}

void GDX12DeviceFactory::Reset()
{
    _dxgiFactory.Reset();
    _isInitialized = false;
}

std::vector<DeviceDesc> GDX12DeviceFactory::GetDeviceDescriptors()
{
    if (!_isInitialized) { ThrowIfFailed(Initialize()); }

    std::vector<DeviceDesc> devices;
    std::vector<UINT> addedAdapters;

    ComPtr<IDXGIAdapter1> adapter1;
    UINT AdapterIndex = 0;

    while (_dxgiFactory->EnumAdapterByGpuPreference(AdapterIndex, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
        IID_PPV_ARGS(&adapter1)) != DXGI_ERROR_NOT_FOUND)
    {
        ComPtr<IDXGIAdapter4> adapter4;
        DXGI_ADAPTER_DESC1 adapterDesc;

        if (FAILED(adapter1.As(&adapter4)) || 
            FAILED(adapter4->GetDesc1(&adapterDesc)) || 
            adapterDesc.Flags == DXGI_ADAPTER_FLAG3_SOFTWARE || 
            std::find(addedAdapters.begin(), addedAdapters.end(), adapterDesc.DeviceId) != addedAdapters.end())
        {
            AdapterIndex++;
            continue;
        }

        DeviceDesc desc = {};
        desc.Adapter = adapter4;
        desc.Name = adapterDesc.Description;
        desc.DedicatedVideoMemory = adapterDesc.DedicatedVideoMemory;
        desc.DedicatedSystemMemory = adapterDesc.DedicatedSystemMemory;
        desc.SharedSystemMemory = adapterDesc.SharedSystemMemory;

        devices.push_back(desc);
        addedAdapters.push_back(adapterDesc.DeviceId);

        AdapterIndex++;
    }

    return devices;
}

ComPtr<IDXGIAdapter4> GDX12DeviceFactory::GetDefaultAdapter()
{
    if (!_isInitialized) { ThrowIfFailed(Initialize()); }

    ComPtr<IDXGIAdapter4> defaultAdapter;
    _dxgiFactory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_UNSPECIFIED, IID_PPV_ARGS(&defaultAdapter));
    return defaultAdapter;
}

ComPtr<IDXGIAdapter4> GDX12DeviceFactory::GetMostPerformantAdapter()
{
    auto descs = GetDeviceDescriptors();
    if (descs.empty()) { OutputDebugStringA("ERROR: No suitable devices found.\n"); }
    return GetDeviceDescriptors()[0].Adapter;
}

ComPtr<IDXGISwapChain4> GDX12DeviceFactory::CreateSwapChain(GDX12Device* device, DXGI_SWAP_CHAIN_DESC1& desc, HWND hwnd)
{
    ComPtr<IDXGISwapChain4> swapChain4;

    ComPtr<IDXGISwapChain1> swapChain1;
    ThrowIfFailed(_dxgiFactory->CreateSwapChainForHwnd(device->GetCommandQueue()->GetCommandQueue().Get(),
        hwnd, &desc, nullptr, nullptr, &swapChain1));

    ThrowIfFailed(swapChain1.As(&swapChain4));

    return swapChain4;
}

#include "Engine.RendererDX12/GDX12DeviceFactory.h"

#include "Engine.RendererDX12/GDX12Device.h"
#include "Engine.RendererDX12/GDX12CommandQueue.h"
#include "Engine.RendererDX12/GDX12StreamlineSDK.h"

#include <algorithm>

ComPtr<IDXGIFactory7> GDX12DeviceFactory::_dxgiFactory = nullptr;
ComPtr<IDXGIFactory7> GDX12DeviceFactory::_dxgiFactoryProxy = nullptr;
bool GDX12DeviceFactory::_isInitialized = false;

HRESULT GDX12DeviceFactory::Initialize()
{
    if (_isInitialized)
    {
        if (!_dxgiFactoryProxy && GDX12StreamlineSDK::Get().IsEnabled())
        {
            _dxgiFactoryProxy = GDX12StreamlineSDK::Get().CreateProxyFactory(_dxgiFactory);
        }
        return S_OK;
    }

    ComPtr<IDXGIFactory7> createdFactory;
    HRESULT hr = CreateDXGIFactory2(D3D12_DEVICE_FACTORY_FLAG_ALLOW_RETURNING_EXISTING_DEVICE, IID_PPV_ARGS(&createdFactory));

    if (SUCCEEDED(hr))
    {
        _dxgiFactoryProxy = GDX12StreamlineSDK::Get().CreateProxyFactory(createdFactory);
        _dxgiFactory = GDX12StreamlineSDK::Get().GetNativeFactory(_dxgiFactoryProxy ? _dxgiFactoryProxy : createdFactory);
        _isInitialized = true;
    }
    return hr;
}

const ComPtr<IDXGIFactory7>& GDX12DeviceFactory::GetFactory()
{
    if (!_isInitialized) { ThrowIfFailed(Initialize()); }
    return _dxgiFactory;
}

void GDX12DeviceFactory::Reset()
{
    _dxgiFactoryProxy.Reset();
    _dxgiFactory.Reset();
    _isInitialized = false;
}

std::vector<DeviceDesc> GDX12DeviceFactory::GetDeviceDescriptors()
{
    if (!_isInitialized) { ThrowIfFailed(Initialize()); }

    std::vector<DeviceDesc> devices;
    std::vector<LUID> adapterIds;
    ComPtr<IDXGIAdapter1> adapter1;
    for (UINT adapterIndex = 0;; ++adapterIndex)
    {
        const auto result = _dxgiFactory->EnumAdapterByGpuPreference(adapterIndex, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
            IID_PPV_ARGS(adapter1.ReleaseAndGetAddressOf()));
        if (result == DXGI_ERROR_NOT_FOUND) { break; }
        ThrowIfFailed(result);

        ComPtr<IDXGIAdapter4> adapter4;
        DXGI_ADAPTER_DESC1 adapterDesc;

        if (FAILED(adapter1.As(&adapter4)) || 
            FAILED(adapter4->GetDesc1(&adapterDesc)) || 
            (adapterDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
        {
            continue;
        }
        const auto luid = adapterDesc.AdapterLuid;
        if (std::any_of(adapterIds.begin(), adapterIds.end(), [luid](const LUID& id)
            { return id.LowPart == luid.LowPart && id.HighPart == luid.HighPart; }))
        { continue; }

        DeviceDesc desc = {};
        desc.Adapter = adapter4;
        desc.Name = adapterDesc.Description;
        desc.DedicatedVideoMemory = adapterDesc.DedicatedVideoMemory;
        desc.DedicatedSystemMemory = adapterDesc.DedicatedSystemMemory;
        desc.SharedSystemMemory = adapterDesc.SharedSystemMemory;

        devices.push_back(desc);
        adapterIds.push_back(luid);
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
    return descs.empty() ? nullptr : descs.front().Adapter;
}

ComPtr<IDXGISwapChain4> GDX12DeviceFactory::CreateSwapChain(GDX12Device* device, DXGI_SWAP_CHAIN_DESC1& desc, HWND hwnd)
{
    ComPtr<IDXGISwapChain4> swapChain4;
    const ComPtr<IDXGIFactory7>& factory = _dxgiFactoryProxy ? _dxgiFactoryProxy : _dxgiFactory;

    ComPtr<IDXGISwapChain1> swapChain1;
    ThrowIfFailed(factory->CreateSwapChainForHwnd(device->GetCommandQueue()->GetPresentCommandQueue().Get(),
        hwnd, &desc, nullptr, nullptr, &swapChain1));

    ThrowIfFailed(swapChain1.As(&swapChain4));

    return swapChain4;
}

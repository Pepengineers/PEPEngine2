#include "Engine.RendererDX12/GDX12DeviceFactory.h"

#include "Engine.RendererDX12/GDX12Device.h"
#include "Engine.RendererDX12/GDX12CommandQueue.h"
#include "Engine.RendererDX12/GDX12StreamlineSDK.h"
#include <algorithm>
#include <d3dkmthk.h>
#pragma comment(lib, "gdi32.lib")

namespace
{
bool IsIndirectDisplayAdapter(LUID luid)
{
    D3DKMT_OPENADAPTERFROMLUID adapter{};
    adapter.AdapterLuid = luid;
    if (D3DKMTOpenAdapterFromLuid(&adapter) < 0) return false;
    D3DKMT_ADAPTERTYPE type{};
    D3DKMT_QUERYADAPTERINFO query{};
    query.hAdapter = adapter.hAdapter;
    query.Type = KMTQAITYPE_ADAPTERTYPE;
    query.pPrivateDriverData = &type;
    query.PrivateDriverDataSize = sizeof(type);
    const auto status = D3DKMTQueryAdapterInfo(&query);
    D3DKMT_CLOSEADAPTER close{};
    close.hAdapter = adapter.hAdapter;
    D3DKMTCloseAdapter(&close);
    return status >= 0 && type.IndirectDisplayDevice;
}
}

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
    for (UINT adapterIndex = 0;; ++adapterIndex)
    {
        ComPtr<IDXGIAdapter1> adapter;
        const auto result = _dxgiFactory->EnumAdapterByGpuPreference(adapterIndex,
            DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
        if (result == DXGI_ERROR_NOT_FOUND) break;
        ThrowIfFailed(result);
        ComPtr<IDXGIAdapter4> adapter4;
        DXGI_ADAPTER_DESC1 adapterDesc{};
        if (FAILED(adapter.As(&adapter4)) || FAILED(adapter->GetDesc1(&adapterDesc))) continue;
        if (adapterDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if (IsIndirectDisplayAdapter(adapterDesc.AdapterLuid)) continue;
        if (std::any_of(devices.begin(), devices.end(), [&](const DeviceDesc& desc)
            { return desc.AdapterLuid.HighPart == adapterDesc.AdapterLuid.HighPart &&
                desc.AdapterLuid.LowPart == adapterDesc.AdapterLuid.LowPart; })) continue;

        DeviceDesc desc = {};
        desc.Adapter = adapter4;
        desc.AdapterLuid = adapterDesc.AdapterLuid;
        desc.Name = adapterDesc.Description;
        desc.DedicatedVideoMemory = adapterDesc.DedicatedVideoMemory;
        desc.DedicatedSystemMemory = adapterDesc.DedicatedSystemMemory;
        desc.SharedSystemMemory = adapterDesc.SharedSystemMemory;

        devices.push_back(desc);
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
    const auto descs = GetDeviceDescriptors();
    if (descs.empty()) ThrowIfFailed(DXGI_ERROR_NOT_FOUND);
    return descs.front().Adapter;
}

ComPtr<IDXGISwapChain4> GDX12DeviceFactory::CreateSwapChain(GDX12Device* device, DXGI_SWAP_CHAIN_DESC1& desc, const HWND hwnd)
{
    ComPtr<IDXGISwapChain4> swapChain4;
    const ComPtr<IDXGIFactory7>& factory = _dxgiFactoryProxy ? _dxgiFactoryProxy : _dxgiFactory;

    ComPtr<IDXGISwapChain1> swapChain1;
    ThrowIfFailed(factory->CreateSwapChainForHwnd(device->GetCommandQueue()->GetPresentCommandQueue().Get(),
        hwnd, &desc, nullptr, nullptr, &swapChain1));

    ThrowIfFailed(swapChain1.As(&swapChain4));

    return swapChain4;
}

#include "Engine.RendererDX12/GDX12DeviceFactory.h"

#include "Engine.RendererDX12/GDX12Device.h"
#include "Engine.RendererDX12/GDX12CommandQueue.h"
#include "Engine.RendererDX12/GDX12StreamlineSDK.h"

#include <d3dkmthk.h>

#include <algorithm>

#pragma comment(lib, "Gdi32.lib")

namespace
{
    struct AdapterIdentity
    {
        LUID Luid;
        std::wstring PhysicalKey;
        bool IndirectDisplay = false;
    };

    AdapterIdentity GetAdapterIdentity(LUID luid)
    {
        AdapterIdentity identity{luid, {}};
        D3DKMT_OPENADAPTERFROMLUID opened{luid};
        if (D3DKMTOpenAdapterFromLuid(&opened) != 0) { return identity; }

        // A logical adapter for an indirect display can use the same physical render GPU.
        D3DKMT_ADAPTERTYPE type{};
        D3DKMT_QUERYADAPTERINFO query{opened.hAdapter, KMTQAITYPE_ADAPTERTYPE, &type, sizeof(type)};
        if (D3DKMTQueryAdapterInfo(&query) == 0) { identity.IndirectDisplay = type.IndirectDisplayDevice != 0; }

        D3DKMT_PHYSICAL_ADAPTER_COUNT count{};
        query = {opened.hAdapter, KMTQAITYPE_PHYSICALADAPTERCOUNT, &count, sizeof(count)};
        WCHAR key[512]{};
        if (D3DKMTQueryAdapterInfo(&query) == 0 && count.Count == 1)
        {
            UINT length = _countof(key);
            D3DKMT_QUERY_PHYSICAL_ADAPTER_PNP_KEY pnp{0, D3DKMT_PNP_KEY_HARDWARE, key, &length};
            query = {opened.hAdapter, KMTQAITYPE_PHYSICALADAPTERPNPKEY, &pnp, sizeof(pnp)};
            if (D3DKMTQueryAdapterInfo(&query) != 0) { key[0] = L'\0'; }
        }

        D3DKMT_CLOSEADAPTER close{opened.hAdapter};
        D3DKMTCloseAdapter(&close);
        identity.PhysicalKey = key;
        return identity;
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
    std::vector<AdapterIdentity> adapterIds;
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
        if (std::any_of(adapterIds.begin(), adapterIds.end(), [luid](const AdapterIdentity& id)
            { return id.Luid.LowPart == luid.LowPart && id.Luid.HighPart == luid.HighPart; }))
        { continue; }

        auto identity = GetAdapterIdentity(luid);
        const auto duplicate = std::find_if(adapterIds.begin(), adapterIds.end(), [&identity](const AdapterIdentity& id)
        { return !identity.PhysicalKey.empty() && identity.PhysicalKey == id.PhysicalKey; });
        if (duplicate != adapterIds.end() && (!duplicate->IndirectDisplay || identity.IndirectDisplay))
        { continue; }

        DeviceDesc desc = {};
        desc.Adapter = adapter4;
        desc.Name = adapterDesc.Description;
        desc.DedicatedVideoMemory = adapterDesc.DedicatedVideoMemory;
        desc.DedicatedSystemMemory = adapterDesc.DedicatedSystemMemory;
        desc.SharedSystemMemory = adapterDesc.SharedSystemMemory;

        if (duplicate != adapterIds.end())
        {
            devices[duplicate - adapterIds.begin()] = std::move(desc);
            *duplicate = std::move(identity);
        }
        else
        {
            devices.push_back(std::move(desc));
            adapterIds.push_back(std::move(identity));
        }
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

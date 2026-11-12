#include "Engine.RendererDX12/GDX12Device.h"

#include "Engine.RendererDX12/GDX12CommandQueue.h"
#include "Engine.RendererDX12/GDX12StreamlineSDK.h"

#include <cfgmgr32.h>
#include <initguid.h>
#include <devpkey.h>
#include <pciprop.h>

#pragma comment(lib, "Cfgmgr32.lib")

namespace
{
    std::string ToUTF8(const wchar_t* text)
    {
        const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, nullptr, 0, nullptr, nullptr);
        if (size <= 1) { return {}; }

        std::string result(size, '\0');
        if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, result.data(), size, nullptr, nullptr))
        { return {}; }
        result.pop_back();
        return result;
    }

    unsigned GetPCIeGeneration(LUID adapterLuid)
    {
        DISPLAYCONFIG_ADAPTER_NAME adapter{};
        adapter.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADAPTER_NAME;
        adapter.header.size = sizeof(adapter);
        adapter.header.adapterId = adapterLuid;
        if (DisplayConfigGetDeviceInfo(&adapter.header) != ERROR_SUCCESS) { return 0; }

        WCHAR instanceId[MAX_DEVICE_ID_LEN]{};
        DEVPROPTYPE type = DEVPROP_TYPE_EMPTY;
        ULONG size = sizeof(instanceId);
        if (CM_Get_Device_Interface_PropertyW(adapter.adapterDevicePath, &DEVPKEY_Device_InstanceId, &type,
                reinterpret_cast<PBYTE>(instanceId), &size, 0) != CR_SUCCESS || type != DEVPROP_TYPE_STRING)
        { return 0; }

        DEVINST device = 0;
        if (CM_Locate_DevNodeW(&device, instanceId, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) { return 0; }

        ULONG generation = 0;
        size = sizeof(generation);
        if (CM_Get_DevNode_PropertyW(device, &DEVPKEY_PciDevice_CurrentLinkSpeed, &type,
                reinterpret_cast<PBYTE>(&generation), &size, 0) != CR_SUCCESS ||
            type != DEVPROP_TYPE_UINT32 || size != sizeof(generation) || generation > 7)
        { return 0; }
        return generation;
    }
}

GDX12Device::GDX12Device() :
    _isInitialized(false),
    _rtvDescriptorSize(0),
    _dsvDescriptorSize(0),
    _cbvSrvUavDescriptorSize(0),
    Role(DEVICE_ROLE_PRIMARY)
{
}

GDX12Device::~GDX12Device() = default;

HRESULT GDX12Device::Initialize(ComPtr<IDXGIAdapter4> adapter)
{
    HRESULT hr = E_FAIL;
    D3D_FEATURE_LEVEL testFeatureLevels[] = 
    {
        D3D_FEATURE_LEVEL_12_2,
        D3D_FEATURE_LEVEL_12_1,
        D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0
    };

    for (auto level : testFeatureLevels)
    {
        hr = D3D12CreateDevice(adapter.Get(), level, IID_PPV_ARGS(&_device));
        if (SUCCEEDED(hr)) 
        { 
            _adapter = adapter;
            _specs.MaxFeatureLevel = level;
            break; 
        }
    }
    if (FAILED(hr))
    {
        OutputDebugStringA("ERROR: FAILED TO CREATE DX12DEVICE\n");
        return hr;
    }

    _proxyDevice = GDX12StreamlineSDK::Get().CreateProxyDevice(_device, Role == DEVICE_ROLE_PRIMARY);
    _device = GDX12StreamlineSDK::Get().GetNativeDevice(_proxyDevice ? _proxyDevice : _device);
    CollectDeviceFeatures();

    _commandQueue = std::make_unique<GDX12CommandQueue>(this);
    _copyCommandQueue = std::make_unique<GDX12CommandQueue>(this, D3D12_COMMAND_LIST_TYPE_COPY);

    _isInitialized = true;

    return hr;
}

void GDX12Device::CollectDeviceFeatures()
{
    _rtvDescriptorSize = _device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    _dsvDescriptorSize = _device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    _cbvSrvUavDescriptorSize = _device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    _specs.Features.Init(_device.Get());

    DXGI_ADAPTER_DESC1 adapterDesc{};
    const HRESULT adapterResult = _adapter->GetDesc1(&adapterDesc);

    _specs.Name = ToUTF8(adapterDesc.Description);
    _specs.DedicatedVideoMemory = adapterDesc.DedicatedVideoMemory;
    _specs.DedicatedSystemMemory = adapterDesc.DedicatedSystemMemory;
    _specs.SharedSystemMemory = adapterDesc.SharedSystemMemory;
    _specs.PCIeGeneration = SUCCEEDED(adapterResult) && !_specs.Features.UMA()
        ? GetPCIeGeneration(adapterDesc.AdapterLuid)
        : 0;
}

void GDX12Device::Reset()
{
    Flush();
    _copyCommandQueue.reset();
    _commandQueue.reset();
    _proxyDevice.Reset();
    _device.Reset();
    _adapter.Reset();
    _isInitialized = false;
}

const ComPtr<ID3D12Device14>& GDX12Device::GetDevice()
{
    return _device;
}

const ComPtr<ID3D12Device14>& GDX12Device::GetCommandQueueCreationDevice()
{
    return _proxyDevice ? _proxyDevice : _device;
}

GDX12CommandQueue* GDX12Device::GetCommandQueue()
{
    return _commandQueue.get();
}

GDX12CommandQueue* GDX12Device::GetCopyCommandQueue()
{
    return _copyCommandQueue.get();
}

void GDX12Device::Flush()
{
    ThrowIfFailed(TryFlush());
}

HRESULT GDX12Device::TryFlush() noexcept
{
    const HRESULT directResult = _commandQueue ? _commandQueue->TryFlush() : S_OK;
    const HRESULT copyResult = _copyCommandQueue ? _copyCommandQueue->TryFlush() : S_OK;
    return FAILED(directResult) ? directResult : copyResult;
}

const DeviceSpecs& GDX12Device::GetDeviceFeatures() const
{
    return _specs;
}

LUID GDX12Device::GetAdapterLuid() const
{
    LUID luid = {};
    if (!_adapter) { return luid; }

    DXGI_ADAPTER_DESC1 adapterDesc = {};
    if (FAILED(_adapter->GetDesc1(&adapterDesc))) { return luid; }

    return adapterDesc.AdapterLuid;
}

const bool GDX12Device::IsInitialized()
{
    return _isInitialized;
}

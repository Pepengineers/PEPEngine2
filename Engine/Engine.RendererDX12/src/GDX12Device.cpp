#include "Engine.RendererDX12/GDX12Device.h"
#include "Engine.Core/Logging/Logger.h"

#include "Engine.RendererDX12/GDX12CommandQueue.h"
#include "Engine.RendererDX12/GDX12StreamlineSDK.h"

GDX12Device::GDX12Device() :
    _isInitialized(false),
    _rtvDescriptorSize(0),
    _dsvDescriptorSize(0),
    _cbvSrvUavDescriptorSize(0),
    Role(DEVICE_ROLE_PRIMARY)
{
}

GDX12Device::~GDX12Device()
{
    Reset();
}

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
    if (FAILED(hr)) { OutputDebugStringA("ERROR: FAILED TO CREATE DX12DEVICE\n"); return hr; }

    _proxyDevice = GDX12StreamlineSDK::Get().CreateProxyDevice(_device, Role == DEVICE_ROLE_PRIMARY);
    _device = GDX12StreamlineSDK::Get().GetNativeDevice(_proxyDevice ? _proxyDevice : _device);
    CollectDeviceFeatures();

    D3D12MA::ALLOCATOR_DESC allocatorDesc = {};
    allocatorDesc.pDevice = _device.Get();
    allocatorDesc.pAdapter = _adapter.Get();
    ThrowIfFailed(D3D12MA::CreateAllocator(&allocatorDesc, &_allocator));

    _commandQueue = std::make_unique<GDX12CommandQueue>(this);

    _isInitialized = true;

    return hr;
}

void GDX12Device::CollectDeviceFeatures()
{
    _rtvDescriptorSize = _device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    _dsvDescriptorSize = _device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    _cbvSrvUavDescriptorSize = _device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    _specs.Features.Init(_device.Get());

    DXGI_ADAPTER_DESC1 adapterDesc;
    _adapter.Get()->GetDesc1(&adapterDesc);

    _specs.Name = WStringToString(adapterDesc.Description);
    _specs.DedicatedVideoMemory = adapterDesc.DedicatedVideoMemory;
    _specs.DedicatedSystemMemory = adapterDesc.DedicatedSystemMemory;
    _specs.SharedSystemMemory = adapterDesc.SharedSystemMemory;
    Log::Info("Renderer", "GPU adapter: ", adapterDesc.Description, ", dedicated video memory ",
        adapterDesc.DedicatedVideoMemory / (1024u * 1024u), " MiB");
}

void GDX12Device::Reset()
{
    _commandQueue.reset();
    _allocator.Reset();
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

const DeviceSpecs& GDX12Device::GetDeviceFeatures()
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

GDX12ResourceHandle GDX12Device::CreateResource(D3D12_HEAP_TYPE heapType, const D3D12_RESOURCE_DESC& desc,
    D3D12_RESOURCE_STATES initialState, const D3D12_CLEAR_VALUE* clearValue)
{
    D3D12MA::ALLOCATION_DESC allocationDesc = {};
    allocationDesc.HeapType = heapType;
    ComPtr<D3D12MA::Allocation> allocation;
    ThrowIfFailed(_allocator->CreateResource(&allocationDesc, &desc, initialState, clearValue,
        &allocation, IID_NULL, nullptr));
    return GDX12ResourceHandle(_allocator, std::move(allocation));
}

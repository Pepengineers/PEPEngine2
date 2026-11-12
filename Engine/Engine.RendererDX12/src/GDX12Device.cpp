#include "Engine.RendererDX12/GDX12Device.h"
#include "Engine.Core/Logging/Logger.h"
#include <exception>
#include <stdexcept>

#include "Engine.RendererDX12/GDX12CommandQueue.h"
#include "Engine.RendererDX12/GDX12StreamlineSDK.h"

extern "C"
{
    __declspec(dllexport) extern const UINT D3D12SDKVersion = 619;
    __declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";
}

GDX12Device::GDX12Device() :
    _isInitialized(false),
    _rtvDescriptorSize(0),
    _dsvDescriptorSize(0),
    _cbvSrvUavDescriptorSize(0)
{
}

GDX12Device::~GDX12Device()
{
    try { Reset(); }
    catch (const DxException& error) { OutputDebugStringW(error.ToString().c_str()); }
    catch (const std::exception& error) { OutputDebugStringA(error.what()); }
    catch (...) { OutputDebugStringA("GDX12Device shutdown failed; call Reset explicitly to observe errors.\n"); }
}

HRESULT GDX12Device::Initialize(ComPtr<IDXGIAdapter4> adapter)
{
    Reset();
    if (!adapter) return E_INVALIDARG;
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

    CollectDeviceFeatures();
    if (_specs.Features.HighestShaderModel() < D3D_SHADER_MODEL_6_0)
    {
        Log::Error("Renderer", "GPU adapter ", _specs.Name, " requires Shader Model 6.0 or newer; reported ",
            ShaderModelToString(_specs.Features.HighestShaderModel()));
        Reset();
        return DXGI_ERROR_UNSUPPORTED;
    }
    _proxyDevice = GDX12StreamlineSDK::Get().CreateProxyDevice(_device, true);
    _device = GDX12StreamlineSDK::Get().GetNativeDevice(_proxyDevice ? _proxyDevice : _device);

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

    ThrowIfFailed(_specs.Features.Init(_device.Get()));

    DXGI_ADAPTER_DESC1 adapterDesc;
    ThrowIfFailed(_adapter->GetDesc1(&adapterDesc));

    _specs.Name = WStringToString(adapterDesc.Description);
    _specs.DedicatedVideoMemory = adapterDesc.DedicatedVideoMemory;
    _specs.DedicatedSystemMemory = adapterDesc.DedicatedSystemMemory;
    _specs.SharedSystemMemory = adapterDesc.SharedSystemMemory;
    Log::Info("Renderer", "GPU adapter: ", adapterDesc.Description, ", dedicated video memory ",
        adapterDesc.DedicatedVideoMemory / (1024u * 1024u), " MiB");
}

void GDX12Device::Reset()
{
    _computeQueue.reset();
    _computeQueueAttempted = false;
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

GDX12CommandQueue* GDX12Device::TryGetComputeQueue()
{
    if (!_isInitialized) throw std::logic_error("COMPUTE queue requires an initialized device");
    if (!_computeQueueAttempted)
    {
        _computeQueueAttempted = true;
        try { _computeQueue = std::make_unique<GDX12CommandQueue>(this, D3D12_COMMAND_LIST_TYPE_COMPUTE); }
        catch (const DxException& error)
        {
            ThrowIfFailed(_device->GetDeviceRemovedReason());
            Log::Warning("Renderer", "COMPUTE queue unavailable: ", error.ToString(), "; using DIRECT particle simulation");
        }
    }
    return _computeQueue.get();
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
    ThrowIfFailed(_adapter->GetDesc1(&adapterDesc));

    return adapterDesc.AdapterLuid;
}

const bool GDX12Device::IsInitialized()
{
    return _isInitialized;
}

GDX12ResourceHandle GDX12Device::CreateResource(const D3D12_HEAP_TYPE heapType, const D3D12_RESOURCE_DESC& desc,
    const D3D12_RESOURCE_STATES initialState, const D3D12_CLEAR_VALUE* clearValue)
{
    D3D12MA::ALLOCATION_DESC allocationDesc = {};
    allocationDesc.HeapType = heapType;
    ComPtr<D3D12MA::Allocation> allocation;
    ThrowIfFailed(_allocator->CreateResource(&allocationDesc, &desc, initialState, clearValue,
        &allocation, IID_NULL, nullptr));
    return GDX12ResourceHandle(_allocator, std::move(allocation));
}

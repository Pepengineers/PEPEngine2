#include "Engine.RendererDX12/GDX12Device.h"

#include "Engine.RendererDX12/GDX12CommandQueue.h"

GDX12Device::GDX12Device() :
    _isInitialized(false),
    _rtvDescriptorSize(0),
    _dsvDescriptorSize(0),
    _cbvSrvUavDescriptorSize(0)
{
}

GDX12Device::~GDX12Device()
{
    Reset();
}

HRESULT GDX12Device::Initialize(ComPtr<IDXGIAdapter4> adapter)
{
    Reset();

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
    if (FAILED(hr)) { OutputDebugStringA("ERROR: FAILED TO CREATE DX12DEVICE\n"); }

    CollectDeviceFeatures();

    _commandQueue = std::make_shared<GDX12CommandQueue>(this);

    return hr;
}

void GDX12Device::CollectDeviceFeatures()
{
    _isInitialized = true;

    _rtvDescriptorSize = _device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    _dsvDescriptorSize = _device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    _cbvSrvUavDescriptorSize = _device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    DXGI_ADAPTER_DESC1 adapterDesc;
    _adapter.Get()->GetDesc1(&adapterDesc);

    _specs.Name = WStringToString(adapterDesc.Description);
    _specs.DedicatedVideoMemory = adapterDesc.DedicatedVideoMemory;
    _specs.DedicatedSystemMemory = adapterDesc.DedicatedSystemMemory;
    _specs.SharedSystemMemory = adapterDesc.SharedSystemMemory;

    // Query shader model support
    D3D12_FEATURE_DATA_SHADER_MODEL shaderModel;
    D3D_SHADER_MODEL testModels[] =
    {
        D3D_SHADER_MODEL_6_10, // these two versions
        D3D_SHADER_MODEL_6_9,  // may not be compilable by DXC, so we might ditch them later
        D3D_SHADER_MODEL_6_8,
        D3D_SHADER_MODEL_6_7,
        D3D_SHADER_MODEL_6_6,
        D3D_SHADER_MODEL_6_5,
        D3D_SHADER_MODEL_6_4,
        D3D_SHADER_MODEL_6_3,
        D3D_SHADER_MODEL_6_2,
        D3D_SHADER_MODEL_6_1,
        D3D_SHADER_MODEL_6_0,
        D3D_SHADER_MODEL_5_1 };

    for (auto model : testModels)
    {
        shaderModel.HighestShaderModel = model;
        if (SUCCEEDED(_device->CheckFeatureSupport(
            D3D12_FEATURE_SHADER_MODEL,
            &shaderModel,
            sizeof(shaderModel))))
        {
            _specs.MaxShaderModel = model;
            break;
        }
    }

    // Query raytracing support
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5 = {};
    if (SUCCEEDED(_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5))))
    {
        _specs.RaytracingSupport = (options5.RaytracingTier != D3D12_RAYTRACING_TIER_NOT_SUPPORTED);
    }

    // Query mesh shaders support
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7 = {};
    if (SUCCEEDED(_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options7, sizeof(options7))))
    {
        _specs.MeshShadersSupport = (options7.MeshShaderTier != D3D12_MESH_SHADER_TIER_NOT_SUPPORTED);
    }

    // Query variable rate shading support
    D3D12_FEATURE_DATA_D3D12_OPTIONS6 options6 = {};
    if (SUCCEEDED(_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &options6, sizeof(options6))))
    {
        _specs.VariableRateShadingSupport = (options6.VariableShadingRateTier != D3D12_VARIABLE_SHADING_RATE_TIER_NOT_SUPPORTED);
    }

    // Query enhanced barriers support
    D3D12_FEATURE_DATA_D3D12_OPTIONS12 options12 = {};
    if (SUCCEEDED(_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &options12, sizeof(options12))))
    {
        _specs.EnhancedBarriersSupport = options12.EnhancedBarriersSupported;
    }
}

void GDX12Device::Reset()
{
    _device.Reset();
    _isInitialized = false;
    _rtvDescriptorSize = 0;
    _dsvDescriptorSize = 0;
    _cbvSrvUavDescriptorSize = 0;
}

ComPtr<ID3D12Device14> GDX12Device::GetDevice()
{
    return _device;
}

std::shared_ptr<GDX12CommandQueue> GDX12Device::GetCommandQueue()
{
    return _commandQueue;
}

const DeviceSpecs& GDX12Device::GetDeviceFeatures() const
{
    return _specs;
}

const bool GDX12Device::IsInitialized() const
{
    return _isInitialized;
}
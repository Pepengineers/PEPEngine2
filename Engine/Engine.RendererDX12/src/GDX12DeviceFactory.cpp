#include "Engine.RendererDX12/GDX12DeviceFactory.h"

ComPtr<IDXGIFactory7> GDX12DeviceFactory::_dxgiFactory = nullptr;
bool GDX12DeviceFactory::_isInitialized = false;

HRESULT GDX12DeviceFactory::Initialize()
{
    if (_isInitialized) { return S_OK; }

    HRESULT hr = CreateDXGIFactory2(D3D12_DEVICE_FACTORY_FLAG_ALLOW_RETURNING_EXISTING_DEVICE, IID_PPV_ARGS(&_dxgiFactory));

    if (SUCCEEDED(hr)) { _isInitialized = true; }
    return hr;
}

ComPtr<IDXGIFactory7> GDX12DeviceFactory::GetFactory()
{
    if (!_isInitialized) { Initialize(); }
    return _dxgiFactory;
}

void GDX12DeviceFactory::Reset()
{
    _dxgiFactory.Reset();
    _isInitialized = false;
}

std::vector<DeviceDesc> GDX12DeviceFactory::GetDeviceDescriptors()
{
    if (!_isInitialized) { Initialize(); }

    std::vector<DeviceDesc> devices;

    ComPtr<IDXGIAdapter4> defaultAdapter;
    DXGI_ADAPTER_DESC1 defaultAdapterdesc = DXGI_ADAPTER_DESC1();

    //get default adapter
    if (SUCCEEDED(_dxgiFactory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_UNSPECIFIED, IID_PPV_ARGS(&defaultAdapter))))
    {
        DeviceDesc desc;
        desc.Adapter = defaultAdapter;

        if (SUCCEEDED(defaultAdapter->GetDesc1(&defaultAdapterdesc)))
        {
            desc.Name = defaultAdapterdesc.Description;
            desc.DedicatedVideoMemory = defaultAdapterdesc.DedicatedVideoMemory;
            desc.DedicatedSystemMemory = defaultAdapterdesc.DedicatedSystemMemory;
            desc.SharedSystemMemory = defaultAdapterdesc.SharedSystemMemory;

            devices.push_back(desc);
        }
    }

    // get all other adapters
    ComPtr<IDXGIAdapter1> adapter1;
    UINT adapterIndex = 0;
    while (_dxgiFactory->EnumAdapters1(adapterIndex, &adapter1) != DXGI_ERROR_NOT_FOUND)
    {
        ComPtr<IDXGIAdapter4> adapter4;
        if (SUCCEEDED(adapter1.As(&adapter4)))
        {
            DXGI_ADAPTER_DESC1 adapterDesc;
            if (SUCCEEDED(adapter4->GetDesc1(&adapterDesc)))
            {
                //skip default device(already added)
                if (adapterDesc.DeviceId == defaultAdapterdesc.DeviceId || adapterDesc.Flags == DXGI_ADAPTER_FLAG3_SOFTWARE)
                { 
                    adapterIndex++;
                    continue;
                }

                DeviceDesc desc;
                desc.Adapter = adapter4;
                desc.Name = adapterDesc.Description;
                desc.DedicatedVideoMemory = adapterDesc.DedicatedVideoMemory;
                desc.DedicatedSystemMemory = adapterDesc.DedicatedSystemMemory;
                desc.SharedSystemMemory = adapterDesc.SharedSystemMemory;

                devices.push_back(desc);
            }
        }

        adapter1.Reset();
        adapterIndex++;
    }

    return devices;
}

ComPtr<IDXGIAdapter4> GDX12DeviceFactory::GetDefaultAdapter()
{
    ComPtr<IDXGIAdapter4> defaultAdapter;
    _dxgiFactory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_UNSPECIFIED, IID_PPV_ARGS(&defaultAdapter));
    return defaultAdapter;
}

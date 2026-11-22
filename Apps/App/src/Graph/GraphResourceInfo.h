#pragma once
#include "Engine.RendererDX12/GDX12Device.h"
#include <stdexcept>

namespace BistroRender
{
// Stored on the native object so destruction and address reuse invalidate the cache.
struct GraphResourceInfo
{
    IUnknown* Owner;
    LUID Adapter;
    D3D12_HEAP_TYPE Heap;
    D3D12_RESOURCE_DIMENSION Dimension;
    D3D12_RESOURCE_FLAGS Flags;

    static GraphResourceInfo Read(ID3D12Resource* resource)
    {
        if (!resource) throw std::invalid_argument("unbound resource");
        static constexpr GUID key{0xb0b8d6a1, 0x8524, 0x4859, {0x8b,0x21,0x65,0x8f,0x35,0xb3,0xcf,0x5a}};
        GraphResourceInfo info{};
        UINT size = sizeof(info);
        const HRESULT hr = resource->GetPrivateData(key, &size, &info);
        if (hr == S_OK && size == sizeof(info)) return info;
        if (hr != DXGI_ERROR_NOT_FOUND) ThrowIfFailed(hr);

        ComPtr<ID3D12Device> device;
        ComPtr<IUnknown> identity;
        ThrowIfFailed(resource->GetDevice(IID_PPV_ARGS(&device)));
        ThrowIfFailed(device.As(&identity));
        D3D12_HEAP_PROPERTIES heap{};
        ThrowIfFailed(resource->GetHeapProperties(&heap, nullptr));
        const auto desc = resource->GetDesc();
        info = {identity.Get(), device->GetAdapterLuid(), heap.Type, desc.Dimension, desc.Flags};
        ThrowIfFailed(resource->SetPrivateData(key, sizeof(info), &info));
        return info;
    }
};
}

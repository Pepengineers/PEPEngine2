#pragma once

#include "Engine.RendererDX12/GDX12Device.h"
#include <map>

namespace BistroRender::ShadowMemory
{
using Bytes = UINT64;
using AdapterKey = std::pair<LONG, DWORD>;

struct Account
{
    Bytes Limit = UINT64_MAX;
    Bytes Allocated = 0;
};

inline auto& For(LUID luid)
{
    static std::map<AdapterKey, Account> accounts;
    return accounts[{luid.HighPart, luid.LowPart}];
}

inline Bytes Size(GDX12Device& device, const D3D12_RESOURCE_DESC& desc)
{
    return device.GetDevice()->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
}

inline auto Depth(UINT side, UINT slices, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)
{
    return CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32_TYPELESS, side, side, UINT16(slices), 1, 1, 0, flags);
}

inline auto View(UINT slices)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R32_FLOAT;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2DArray.MipLevels = 1;
    view.Texture2DArray.ArraySize = slices;
    return view;
}

inline Bytes TransferBytes(GDX12Device& primary, GDX12Device& other, Bytes bytes)
{
    const auto buffer = CD3DX12_RESOURCE_DESC::Buffer(bytes, D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER);
    return (std::max)(Size(primary, buffer), Size(other, buffer));
}

inline Bytes TransferBytes(GDX12Device& primary, GDX12Device& other, UINT side, UINT slices)
{
    const auto shape = Depth(side, slices);
    Bytes bytes = 0;
    primary.GetDevice()->GetCopyableFootprints(&shape, 0, slices, 0, nullptr, nullptr, nullptr, &bytes);
    return TransferBytes(primary, other, bytes);
}

inline bool Fits(GDX12Device& device, Bytes extra)
{
    const auto& account = For(device.GetAdapterLuid());
    return account.Allocated <= account.Limit && extra <= account.Limit - account.Allocated;
}

// Owned alongside the allocation, including transfers retained by a consumer frame's fence.
class Charge
{
public:
    Charge() = default;
    Charge(const Charge&) = delete;
    Charge& operator=(const Charge&) = delete;
    ~Charge() { Reset(); }

    void Set(GDX12Device& device, Bytes bytes)
    {
        Reset();
        _account = &For(device.GetAdapterLuid());
        _bytes = bytes;
        _account->Allocated += bytes;
    }

    Bytes Size() const { return _bytes; }

    void Reset()
    {
        if (_account) _account->Allocated -= _bytes;
        _account = nullptr;
        _bytes = 0;
    }

private:
    Account* _account = nullptr;
    Bytes _bytes = 0;
};
}

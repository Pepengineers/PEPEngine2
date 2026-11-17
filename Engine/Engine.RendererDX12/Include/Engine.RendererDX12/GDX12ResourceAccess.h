#pragma once
#include "Engine.RendererDX12/D3DHelpers.h"
#include <cstdint>
#include <climits>

class GDX12Resource;

enum class GDX12AccessMode : std::uint8_t { Read, Write, ReadWrite };
inline constexpr auto GDX12UnmanagedState = static_cast<D3D12_RESOURCE_STATES>(UINT_MAX);

struct GDX12ResourceAccess
{
    GDX12Resource* Resource;
    GDX12AccessMode Usage;
    D3D12_RESOURCE_STATES State = GDX12UnmanagedState;
};

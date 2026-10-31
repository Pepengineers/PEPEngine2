#pragma once
#include "Engine.RendererDX12/D3DHelpers.h"
#include <cstdint>

enum class GDX12ReflectionProbeMode : int { Inherit = -1, Dynamic, BakedStatic };
struct GDX12ReflectionProbeDesc
{
    uint32_t Entity = 0;
    Vector3 Position = Vector3::Zero;
    Vector3 Extents = Vector3(500);
    float NearPlane = 1, FarPlane = 2000;
    GDX12ReflectionProbeMode Mode = GDX12ReflectionProbeMode::Inherit;
    uint64_t Revision = 0;
};
class GDX12Texture;
bool ReflectionProbeLightingEnabled();
struct GDX12PublishedReflectionProbe
{
    uint64_t World = 0;
    GDX12ReflectionProbeDesc Description;
    GDX12Texture* Texture = nullptr;
    uint64_t Revision = 0;
    UINT MipCount = 0;
};

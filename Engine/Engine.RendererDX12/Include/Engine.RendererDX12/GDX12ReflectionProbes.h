#pragma once
#include "Engine.RendererDX12/D3DHelpers.h"
#include <cstdint>

constexpr UINT MaxReflectionProbes = 32;

struct GDX12ReflectionProbe
{
    uint64_t Id = 0;
    Vector3 Position = Vector3::Zero;
    Vector3 BoxMin = Vector3(-500);
    Vector3 BoxMax = Vector3(500);
    float BlendDistance = 100.0f;
    float Intensity = 1.0f;
    float NearPlane = 1.0f;
    float FarPlane = 10000.0f;
    UINT UpdateInterval = 1;
    UINT Revision = 0;
};

struct GDX12ReflectionSettings
{
    bool Enabled = true;
    int Quality = 2;
    int Implementation = 1;
    UINT Resolution = 128;
    UINT FacesPerFrame = 2;
    UINT PrefilterSamples = 64;
    UINT UpdateInterval = 0;
};

GDX12ReflectionSettings GetReflectionSettings();
bool IsValidReflectionProbe(const GDX12ReflectionProbe& probe);

// Keep the published box and capture centre together until all faces are ready.
struct GDX12ReflectionProbeData
{
    XMFLOAT3 Position = {};
    float Intensity = 0;
    XMFLOAT3 BoxMin = {};
    float BlendDistance = 0;
    XMFLOAT3 BoxMax = {};
    float MaxMip = 0;
};
static_assert(sizeof(GDX12ReflectionProbeData) == 48);

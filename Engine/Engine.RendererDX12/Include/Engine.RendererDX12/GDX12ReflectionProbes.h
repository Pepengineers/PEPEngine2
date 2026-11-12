#pragma once
#include "Engine.RendererDX12/D3DHelpers.h"
#include <cstdint>
#include "Engine.RendererDX12/GDX12ReflectionSchedule.h"

constexpr UINT MaxReflectionProbes = 32;

struct GDX12ReflectionCaptureSettings
{
    int Implementation = 1; // 0 dynamic, 1 static cache + dynamic, 2 frozen static capture.
    UINT Resolution = 0;
    UINT FacesPerFrame = 0;
    UINT PrefilterSamples = 0;
    UINT UpdateInterval = 1;
    UINT SecondaryFaces = 0;
};

struct GDX12ReflectionProbe : GDX12ReflectionCaptureSettings
{
    uint64_t Id = 0;
    Vector3 Position = Vector3::Zero;
    Vector3 BoxMin = Vector3(-500);
    Vector3 BoxMax = Vector3(500);
    float BlendDistance = 100.0f;
    float Intensity = 1.0f;
    float NearPlane = 1.0f;
    float FarPlane = 10000.0f;
    UINT Revision = 0;
};

struct GDX12ReflectionSettings
{
    bool Enabled = true;
    int Quality = 2;
    bool Scheduled = false;
    GDX12ReflectionSchedule::Policy Policy;
};

GDX12ReflectionSettings GetReflectionSettings();
GDX12ReflectionCaptureSettings ResolveReflectionCaptureSettings(const GDX12ReflectionCaptureSettings& author, int quality);
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
    float MinMip = 0;
};
static_assert(sizeof(GDX12ReflectionProbeData) == 52);

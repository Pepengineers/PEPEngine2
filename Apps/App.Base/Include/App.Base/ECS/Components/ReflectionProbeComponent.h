#pragma once
#include "Engine.Core/ECS/Component.h"
#include "Engine.RendererDX12/GDX12ReflectionProbes.h"

struct ReflectionProbeComponent : ComponentTag, GDX12ReflectionCaptureSettings
{
    bool Enabled = true;
    Vector3 Extents = Vector3(500.0f);
    Vector3 CaptureOffset = Vector3::Zero;
    float BlendDistance = 100.0f;
    float Intensity = 1.0f;
    float NearPlane = 1.0f;
    float FarPlane = 10000.0f;
    UINT Revision = 0;

    void Invalidate() { ++Revision; }
};

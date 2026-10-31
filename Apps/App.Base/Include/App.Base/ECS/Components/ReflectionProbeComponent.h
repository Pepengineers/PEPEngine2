#pragma once
#include "Engine.Core/ECS/Component.h"
#include "Engine.RendererDX12/GDX12ReflectionProbes.h"

struct ReflectionProbeComponent : ComponentTag
{
    bool Enabled = true;
    Vector3 Extents = Vector3(500);
    float NearPlane = 1, FarPlane = 2000;
    GDX12ReflectionProbeMode Mode = GDX12ReflectionProbeMode::Inherit;
    uint64_t Revision = 0;
    void Invalidate() { ++Revision; }
};

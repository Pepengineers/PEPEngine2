#pragma once
#include "Engine.Core/ECS/Component.h"
#include "Engine.RendererDX12/D3DHelpers.h"

enum class LightType : uint32_t { Directional, Point, Spot };
enum class ShadowUpdateMode : uint32_t { Cached, EveryFrame };

struct LightComponent : ComponentTag
{
    LightType Type = LightType::Point;
    Vector3 Color = { 1.0f, 0.73f, 0.45f };
    float Intensity = 120.0f; // Candela for local lights; radiance for the directional light.
    float Range = 600.0f;
    float InnerAngle = 20.0f; // Spot half-angles in degrees.
    float OuterAngle = 35.0f;
    bool Enabled = true;
    bool CastShadows = true;
    ShadowUpdateMode ShadowUpdate = ShadowUpdateMode::Cached;
    uint32_t ShadowRevision = 1;

    void InvalidateShadow() { ++ShadowRevision; }
};

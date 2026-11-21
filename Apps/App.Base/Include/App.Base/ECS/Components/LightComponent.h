#pragma once
#include "Engine.Core/ECS/Component.h"
#include "Engine.RendererDX12/GDX12ConstantStructures.h"

struct LightComponent : ComponentTag
{
    LightType Type = LightType::Point;
    Vector3 Color = {1.0f, 0.73f, 0.45f};
    float Intensity = 120.0f;
    float Range = 600.0f; // Centimeters.
    float InnerAngle = 20.0f; // Spot half-angles in degrees.
    float OuterAngle = 35.0f;
    bool Enabled = true;
    bool CastShadows = true;
    bool DirtyFlag = true; // Set after editing light properties directly.

private:
    friend class RenderModule;
    friend class GPUDataUpdateSystem;

    UINT _CBufferIndex = UINT_MAX;
    UINT _numFramesDirty = 0;
};

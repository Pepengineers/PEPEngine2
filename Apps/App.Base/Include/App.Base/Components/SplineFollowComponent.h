#pragma once

#include "Component.h"
#include "Engine.RendererDX12/D3DHelpers.h"
#include "App.Base/Entity.h"

struct SplineFollowComponent : ComponentTag
{
    Entity CurveEntity = InvalidEntity;

    float Time = 0.0f;
    float Duration = 5.0f;

    bool bPlaying = true;
    bool bLoop = false;
    
    SplineFollowComponent() = default;
    
    SplineFollowComponent(
        Entity curveEntity,
        float duration = 5.0f,
        bool bLoop = false,
        bool bPlaying = true,
        float time = 0.0f)
        : CurveEntity{ curveEntity },
          Time{ time },
          Duration{ duration },
          bLoop{ bLoop },
          bPlaying{ bPlaying }
    {
    }
};
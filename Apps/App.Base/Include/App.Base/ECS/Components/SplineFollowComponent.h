#pragma once

#include "Engine.Core/ECS/Component.h"
#include "Engine.Core/ECS/Entity.h"

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
#pragma once

#include "Engine.Core/ECS/Component.h"
#include "Engine.Core/ECS/Entity.h"

#include <cstdint>

enum class SplineUpdateMode : uint8_t
{
    Frame,
    Fixed
};

struct SplineFollowComponent : ComponentTag
{
    Entity CurveEntity = InvalidEntity;

    float Time = 0.0f;
    float Duration = 5.0f;

    bool bPlaying = true;
    bool bLoop = false;

    SplineUpdateMode UpdateMode = SplineUpdateMode::Frame;
    
    SplineFollowComponent() = default;
    
    SplineFollowComponent(
        Entity curveEntity,
        float duration = 5.0f,
        bool bLoop = false,
        bool bPlaying = true,
        float time = 0.0f,
        SplineUpdateMode updateMode = SplineUpdateMode::Frame)
        : CurveEntity{ curveEntity },
          Time{ time },
          Duration{ duration },
          bPlaying{ bPlaying },
          bLoop{ bLoop },
          UpdateMode{ updateMode }
    {
    }
};
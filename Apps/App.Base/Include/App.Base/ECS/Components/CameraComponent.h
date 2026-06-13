#pragma once

#include "Engine.Core/ECS/Component.h"
#include <cstdint>

struct CameraComponent : ComponentTag
{
    float FOV;
    float NearPlane;
    float FarPlane;

    bool DirtyFlag;

    CameraComponent()
        : DirtyFlag(true), _numFramesDirty(0), _CBufferIndex(0), 
        FOV(60.0f), NearPlane(0.1f), FarPlane(10000.f)
    {
    }

    std::uint32_t _CBufferIndex;
    std::uint32_t _numFramesDirty;
};
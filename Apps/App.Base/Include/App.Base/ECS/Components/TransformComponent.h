#pragma once

#include "Engine.Core/ECS/Component.h"
#include <directxtk/SimpleMath.h>
#include <cstdint>

using DirectX::SimpleMath::Vector3;

struct TransformComponent : ComponentTag
{
    Vector3 Location;
    Vector3 Rotation;
    Vector3 Scale;

    bool DirtyFlag;

    TransformComponent(Vector3 location = Vector3(0.f, 0.f, 0.f), 
        Vector3 rotation = Vector3(0.f, 0.f, 0.f), Vector3 scale = Vector3(1.f, 1.f, 1.f)) 
        : Location(location), DirtyFlag(true), _numFramesDirty(0),
        Rotation(rotation), Scale(scale), _CBufferIndex(0)
    {
    }

    std::uint32_t _CBufferIndex;
    std::uint32_t _numFramesDirty;
};
#pragma once

#include "Engine.Core/ECS/Component.h"
#include "Engine.Core/ECS/Entity.h"
#include <directxtk/SimpleMath.h>

using DirectX::SimpleMath::Vector3;

struct LookAtTargetComponent : ComponentTag
{
    Entity TargetEntity = InvalidEntity;

    Vector3 TargetOffset = Vector3(0.0f, 0.0f, 0.0f);

    Vector3 LocalForward = Vector3(0.0f, 0.0f, 1.0f);
    Vector3 WorldUp = Vector3(0.0f, 1.0f, 0.0f);

    bool bEnabled = true;

    LookAtTargetComponent() = default;

    LookAtTargetComponent(
        Entity targetEntity,
        Vector3 targetOffset = Vector3(0.0f, 0.0f, 0.0f),
        Vector3 worldUp = Vector3(0.0f, 1.0f, 0.0f),
        bool enabled = true)
        : TargetEntity{ targetEntity },
          TargetOffset{ targetOffset },
          WorldUp{ worldUp },
          bEnabled{ enabled }
    {
    }
};
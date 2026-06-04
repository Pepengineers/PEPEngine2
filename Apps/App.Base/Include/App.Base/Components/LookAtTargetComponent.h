    #pragma once

#include "Component.h"
#include "Engine.RendererDX12/D3DHelpers.h"
#include "App.Base/Entity.h"

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
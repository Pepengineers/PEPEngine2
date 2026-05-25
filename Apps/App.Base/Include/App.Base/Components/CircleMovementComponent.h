#pragma once

#include "Component.h"
#include "Engine.RendererDX12/D3DHelpers.h"

struct CircleMovementComponent : ComponentTag
{
    float Radius = 100.f;
    float Speed = 100.f;
    
    float CurrentAngle = 0.f;
    Vector3 Center = Vector3(0.f, 0.f, 0.f);
    bool bInitialized = false;

    CircleMovementComponent() = default;

    CircleMovementComponent(float radius, float speed)
        : Radius(radius), Speed(speed)
    {
    }
};
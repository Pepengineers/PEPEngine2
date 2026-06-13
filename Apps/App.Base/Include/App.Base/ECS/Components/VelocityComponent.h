#pragma once

#include "Engine.Core/ECS/Component.h"
#include <directxtk/SimpleMath.h>

using DirectX::SimpleMath::Vector3;

//This should probably be remade into a PhysicsComponent
struct VelocityComponent : ComponentTag
{
    Vector3 Velocity;

    VelocityComponent()
        : Velocity(Vector3(0.f, 0.f, 0.f))
    {
    }

    VelocityComponent(float x, float y, float z)
        : Velocity(Vector3(x, y, z))
    {
    }

    explicit VelocityComponent(const Vector3& velocity)
        : Velocity(velocity)
    {
    }
};
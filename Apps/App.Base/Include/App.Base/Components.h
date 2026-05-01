#pragma once

#include "Engine.RendererDX12/D3DHelpers.h"

struct TransformComponent : ComponentTag
{
    Vector3 Location;
    Vector3 Rotation;
    Vector3 Scale;

    TransformComponent() : Location(Vector3(0.f, 0.f, 0.f)), 
        Rotation(Vector3(0.f, 0.f, 0.f)), Scale(Vector3(1.f, 1.f, 1.f))
    {
    }
};

//This should probably be remade into a PhysicsComponent
struct VelocityComponent : ComponentTag
{
    Vector3 Velocity;

    VelocityComponent() : Velocity(Vector3(0.f, 0.f, 0.f))
    {
    }
};

struct NameComponent : ComponentTag
{
    std::string value;

    NameComponent(const std::string& inValue)
        : value(inValue)
    {
    }
};
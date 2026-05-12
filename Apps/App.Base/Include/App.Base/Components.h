#pragma once

#include "Engine.RendererDX12/D3DHelpers.h"
#include "Engine.RendererDX12/GDX12ConstantStructures.h"
#include "Engine.RendererDX12/GDX12UploadBuffer.h"

struct TransformComponent : ComponentTag
{
    Vector3 Location;
    Vector3 Rotation;
    Vector3 Scale;

    TransformComponent(Vector3 location = Vector3(0.f, 0.f, 0.f), 
        Vector3 rotation = Vector3(0.f, 0.f, 0.f), Vector3 scale = Vector3(1.f, 1.f, 1.f)) 
        : Location(location),
        Rotation(rotation), Scale(scale), _CBufferIndex(0)
    {
    }

    UINT _CBufferIndex;
};

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

struct NameComponent : ComponentTag
{
    std::string value;

    NameComponent(const std::string& inValue)
        : value(inValue)
    {
    }
};
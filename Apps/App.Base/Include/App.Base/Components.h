#pragma once

#include "Engine.RendererDX12/D3DHelpers.h"

#include "Engine.Core/AssetHandles.h"
#include "DirectXCollision.h"
#include "Engine.RendererDX12/GDX12Material.h"

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

    UINT _CBufferIndex;
    UINT _numFramesDirty;
};

struct CameraComponent : ComponentTag
{
    float FOV;
    float NearPlane;
    float FarPlane;

    bool DirtyFlag;

    CameraComponent()
        : DirtyFlag(true), _numFramesDirty(0), _CBufferIndex(0), 
        FOV(45), NearPlane(0.1f), FarPlane(10000.f)
    {
    }

    UINT _CBufferIndex;
    UINT _numFramesDirty;
};

struct StaticMeshRenderComponent : ComponentTag
{
    Engine::Core::MeshHandle MeshHandler;
    std::vector<GDX12Material*> Materials;
    BoundingBox Bounds;

    StaticMeshRenderComponent(Engine::Core::MeshHandle meshHandle, std::vector<GDX12Material*> materials)
        : MeshHandler(meshHandle), Materials(materials)
    {
    }
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
#pragma once

#include <cstdint>
#include <directxtk/SimpleMath.h>
#include <variant>
#include <memory>
#include <vector>
#include <array>

struct PhysicsBodyHandle
{
    uint32_t Value = UINT32_MAX;

    bool IsValid() const
    {
        return Value != UINT32_MAX;
    }
};

enum class PhysicsMotionType : uint8_t
{
    Static,
    Dynamic,
    Kinematic
};

struct BoxShapeDesc
{
    DirectX::SimpleMath::Vector3 HalfExtent{ 0.5f, 0.5f, 0.5f };
};

struct SphereShapeDesc
{
    float Radius = 0.5f;
};

struct CapsuleShapeDesc
{
    float Radius = 0.5f;
    float HalfCylinderHeight = 0.5f;
};

struct CylinderShapeDesc
{
    float Radius = 0.5f;
    float HalfHeight = 0.5f;
};

struct PhysicsTriangleMesh
{
    std::vector<DirectX::SimpleMath::Vector3> Vertices;
    std::vector<std::array<uint32_t, 3>> Triangles;
};

struct TriangleMeshShapeDesc { std::shared_ptr<const PhysicsTriangleMesh> Geometry; };
struct PhysicsConvexCompound
{
    std::vector<std::vector<DirectX::SimpleMath::Vector3>> Hulls;
};
struct ConvexCompoundShapeDesc { std::shared_ptr<const PhysicsConvexCompound> Geometry; };

struct PhysicsWorldSettings
{
    uint32_t MaxBodies = 2048;
    uint32_t MaxBodyPairs = 16384;
    uint32_t MaxContactConstraints = 16384;
    float WorldUnitsPerMeter = 1;
};

using PhysicsShapeDesc = std::variant<BoxShapeDesc, SphereShapeDesc, CapsuleShapeDesc, CylinderShapeDesc, TriangleMeshShapeDesc, ConvexCompoundShapeDesc>;

struct PhysicsBodyDesc
{
    PhysicsShapeDesc Shape = BoxShapeDesc{};
    DirectX::SimpleMath::Vector3 Position{ 0.0f, 0.0f, 0.0f };
    DirectX::SimpleMath::Quaternion Rotation{ 0.0f, 0.0f, 0.0f, 1.0f };
    DirectX::SimpleMath::Vector3 Scale{ 1.0f, 1.0f, 1.0f }; // only for body creation
    PhysicsMotionType MotionType = PhysicsMotionType::Static;
    bool ContinuousCollision = false;
};

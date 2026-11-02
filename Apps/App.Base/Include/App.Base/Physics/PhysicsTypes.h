#pragma once

#include <cstdint>
#include <directxtk/SimpleMath.h>
#include <variant>

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

using PhysicsShapeDesc = std::variant<BoxShapeDesc, SphereShapeDesc, CapsuleShapeDesc, CylinderShapeDesc>;

struct PhysicsBodyDesc
{
    PhysicsShapeDesc Shape = BoxShapeDesc{};
    DirectX::SimpleMath::Vector3 Position{ 0.0f, 0.0f, 0.0f };
    DirectX::SimpleMath::Quaternion Rotation{ 0.0f, 0.0f, 0.0f, 1.0f };
    DirectX::SimpleMath::Vector3 Scale{ 1.0f, 1.0f, 1.0f }; // only for body creation
    PhysicsMotionType MotionType = PhysicsMotionType::Static;
};
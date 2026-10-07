#pragma once

#include <cstdint>
#include <directxtk/SimpleMath.h>

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

struct PhysicsBoxBodyDesc
{
    DirectX::SimpleMath::Vector3 HalfExtent{ 0.5f, 0.5f, 0.5f };
    DirectX::SimpleMath::Vector3 Position{ 0.0f, 0.0f, 0.0f };
    DirectX::SimpleMath::Quaternion Rotation{ 0.0f, 0.0f, 0.0f, 1.0f };
    PhysicsMotionType MotionType = PhysicsMotionType::Static;
};
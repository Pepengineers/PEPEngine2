#pragma once

#include "Engine.Core/ECS/Component.h"
#include <App.Base/Physics/PhysicsTypes.h>

struct RigidBodyComponent : ComponentTag
{
    PhysicsBodyHandle Body;
    PhysicsMotionType MotionType = PhysicsMotionType::Static;
    bool ContinuousCollision = false;
    DirectX::SimpleMath::Quaternion _lastPhysicsRotation;
    DirectX::SimpleMath::Vector3 _lastPhysicsEuler;
    bool _hasPhysicsRotation = false;
};

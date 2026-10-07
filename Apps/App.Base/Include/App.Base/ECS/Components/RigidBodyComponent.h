#pragma once

#include "Engine.Core/ECS/Component.h"
#include <App.Base/Physics/PhysicsTypes.h>

struct RigidBodyComponent : ComponentTag
{
    PhysicsBodyHandle Body;
    PhysicsMotionType MotionType = PhysicsMotionType::Static;
};
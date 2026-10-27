#pragma once

#include "Engine.Core/ECS/Component.h"
#include "App.Base/Physics/PhysicsTypes.h"

struct ColliderComponent : ComponentTag
{
    PhysicsShapeDesc Shape;
};
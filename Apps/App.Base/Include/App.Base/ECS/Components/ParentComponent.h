#pragma once
#include "Engine.Core/ECS/Entity.h"

struct ParentComponent
{
    explicit ParentComponent(Entity parent) : Parent(parent) {}
    Entity Parent;
};

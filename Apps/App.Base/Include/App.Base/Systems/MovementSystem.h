#pragma once

#include "Engine.Core/System.h"
#include "App.Base/ECS/World.h"

class MovementSystem : public System
{
public:
    void Tick(World& world, float dt) override
    {
        WorldECS& ecs = world.GetECS();
        ecs.ForEach<TransformComponent, VelocityComponent>(
            [dt](Entity entity, TransformComponent& transform, VelocityComponent& velocity)
            {
                transform.Location += velocity.Velocity * dt;
                transform.DirtyFlag = true;
            });
    }

};

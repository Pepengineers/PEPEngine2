#pragma once

#include "Engine.Core/System.h"
#include "App.Base/ECS/World.h"

class MovementSystem : public System
{
public:
    void Tick(World& world, float dt) override
    { TickRange(world, dt, 0, GetWorkCount(world)); }

protected:
    size_t GetWorkCount(World& world) const override { return world.GetECS().GetPool<VelocityComponent>().Size(); }
    void TickRange(World& world, float dt, size_t begin, size_t end) override
    {
        WorldECS& ecs = world.GetECS();
        ecs.ForEachRange<VelocityComponent, TransformComponent>(begin, end,
            [dt](Entity entity, VelocityComponent& velocity, TransformComponent& transform)
            {
                transform.Location += velocity.Velocity * dt;
                transform.DirtyFlag = true;
            });
    }

};

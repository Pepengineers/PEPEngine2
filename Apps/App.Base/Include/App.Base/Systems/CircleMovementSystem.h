#pragma once

#include <cmath>

#include "Engine.Core/System.h"
#include "App.Base/ECS/World.h"

class CircleMovementSystem : public System
{
public:
    void Tick(World& world, float dt) override
    { TickRange(world, dt, 0, GetWorkCount(world)); }

protected:
    size_t GetWorkCount(World& world) const override { return world.GetECS().GetPool<CircleMovementComponent>().Size(); }
    void TickRange(World& world, float dt, size_t begin, size_t end) override
    {
        WorldECS& ecs = world.GetECS();

        ecs.ForEachRange<CircleMovementComponent, TransformComponent>(begin, end,
            [dt](Entity entity, CircleMovementComponent& circleComponent, TransformComponent& transform)
            {
                if (!circleComponent.bInitialized)
                {
                    circleComponent.Center = transform.Location;
                    circleComponent.bInitialized = true;
                }

                if (circleComponent.Radius <= 0.0001f)
                {
                    return;
                }

                const float angularSpeed = circleComponent.Speed / circleComponent.Radius;
                circleComponent.CurrentAngle += angularSpeed * dt;

                transform.Location.x = circleComponent.Center.x + std::cos(circleComponent.CurrentAngle) * circleComponent.Radius;
                transform.Location.z = circleComponent.Center.z + std::sin(circleComponent.CurrentAngle) * circleComponent.Radius;

                transform.DirtyFlag = true;
            });
    }
};

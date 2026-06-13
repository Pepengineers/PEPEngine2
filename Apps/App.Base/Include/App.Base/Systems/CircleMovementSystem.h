#pragma once

#include <cmath>

#include "Engine.Core/System.h"
#include "App.Base/ECS/World.h"

class CircleMovementSystem : public System
{
public:
    void Tick(World& world, float dt) override
    {
        WorldECS& ecs = world.GetECS();

        ecs.ForEach<TransformComponent, CircleMovementComponent>(
            [dt](Entity entity, TransformComponent& transform, CircleMovementComponent& circleComponent)
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

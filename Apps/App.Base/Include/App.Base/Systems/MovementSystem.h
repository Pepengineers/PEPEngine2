#pragma once

#include "Engine.Core/System.h"
#include "App.Base/World.h"

class MovementSystem : public System
{
public:
    void Tick(World& world, float dt) override
    {
        WorldECS& ecs = world.GetECS();

        ecs.ForEach<TranslateComponent, VelocityComponent>(
            [dt](Entity entity, TranslateComponent& translate, VelocityComponent& velocity)
            {
                translate.Position += velocity.Velocity * dt;
            });
    }

};
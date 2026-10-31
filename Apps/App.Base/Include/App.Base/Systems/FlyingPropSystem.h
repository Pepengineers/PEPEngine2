#pragma once
#include "Engine.Core/System.h"
#include "App.Base/ECS/World.h"

class FlyingPropSystem final : public System
{
public:
    void Tick(World& world, float dt) override { TickRange(world, dt, 0, GetWorkCount(world)); }
protected:
    size_t GetWorkCount(World& world) const override { return world.GetECS().GetPool<FlyingPropComponent>().Size(); }
    void TickRange(World& world, float dt, size_t begin, size_t end) override
    {
        world.GetECS().ForEachRange<FlyingPropComponent, TransformComponent>(begin, end,
            [dt](Entity, FlyingPropComponent& flight, TransformComponent& transform)
            {
                flight.Time += dt;
                transform.Location = flight.Origin + flight.OffsetAt(flight.Time);
                transform.DirtyFlag = true;
            });
    }
};

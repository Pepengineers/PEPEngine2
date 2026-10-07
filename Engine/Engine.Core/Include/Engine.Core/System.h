#pragma once

#include <cstdint>

class World;

enum class SystemUpdatePhase : uint8_t
{
    FrameUpdate, // per-frame gameplay update
    FixedPrePhysics, // before physics step
    FixedPostPhysics, // after physics step
    RenderPrep // render systems in ecs
};

class System
{
public:
    virtual ~System() = default;

    virtual SystemUpdatePhase GetUpdatePhase() const
    {
        return SystemUpdatePhase::FrameUpdate;
    }
    
    virtual void Tick(World& world, float dt) = 0;
};
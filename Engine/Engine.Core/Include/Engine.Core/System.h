#pragma once

class World;

class System
{
public:
    virtual ~System() = default;

    virtual void Tick(World& world, float dt) = 0;
};
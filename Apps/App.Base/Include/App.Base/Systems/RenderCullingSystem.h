#pragma once

#include "Engine.Core/System.h"
#include "App.Base/World.h"

class RenderCullingSystem final : public System
{
public:
    void Tick(World& world, float dt) override
    {
        // todo:
        // get active camera from world
        // build frustum // vrode bi tut
        // traverse octree
        // write visible  items to render
    }
};
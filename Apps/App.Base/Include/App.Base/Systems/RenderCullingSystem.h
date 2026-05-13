#pragma once

#include "Engine.Core/System.h"
#include "App.Base/World.h"

#include "Engine.Core/BenchmarkEngine.h"
#include "App.Base/Modules/RenderModule.h"

class RenderCullingSystem final : public System
{
public:
    void Tick(World& world, float dt) override
    {
        // todo:
        // get active camera from world
        // traverse octree
        // write visible  items to render

        // just pass all RenderComponents with transforms for now
    }
};
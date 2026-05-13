#pragma once

#include "Engine.Core/System.h"
#include "App.Base/World.h"

#include "Engine.Core/BenchmarkEngine.h"
#include "App.Base/Modules/RenderModule.h"

class RenderSubmitSystem final : public System
{
public:
    void Tick(World& world, float dt) override
    {
        WorldECS& ecs = world.GetECS();
        auto renderModule = BenchmarkEngine::GetLocator().GetModule<RenderModule>();

        CameraComponent* activeCamera = &ecs.Get<CameraComponent>(world.ActiveCamera);

        // since we don't have culling for now, we'll just grab all RenderComponents
        // in the future, this should source the list of visible components from RenderCullingSystem
        ecs.ForEach<StaticMeshRenderComponent, TransformComponent>(
            [&ecs, &renderModule, &activeCamera](Entity entity, StaticMeshRenderComponent& renderer, TransformComponent& transform)
            {

            });
        
    }
};
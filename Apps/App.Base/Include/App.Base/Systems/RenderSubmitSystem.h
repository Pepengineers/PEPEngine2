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

        auto commandRecorder = renderModule->GetCommandRecorder();
        commandRecorder->ClearCommands();

        commandRecorder->DrawFromCamera(activeCamera->_CBufferIndex);

        // Since we don't have culling for now, we'll just grab all RenderComponents.
        // In the future, this should source the list of visible components from RenderCullingSystem
        ecs.ForEach<StaticMeshRenderComponent, TransformComponent>(
            [&ecs, &commandRecorder](Entity entity, StaticMeshRenderComponent& renderer, TransformComponent& transform)
            {
                commandRecorder->DrawMesh(renderer.MeshHandler, renderer.Materials, transform._CBufferIndex);
            });
        
    }
};
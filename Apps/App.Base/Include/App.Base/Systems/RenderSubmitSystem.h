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

        commandRecorder->DrawFromCamera(activeCamera->_CBufferIndex);
    }
};
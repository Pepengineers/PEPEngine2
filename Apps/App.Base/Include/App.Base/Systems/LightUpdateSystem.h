#pragma once
#include "App.Base/ECS/World.h"
#include "App.Base/ECS/WorldLightSnapshot.h"
#include "App.Base/Modules/RenderModule.h"
#include "Engine.Core/BenchmarkEngine.h"
#include "Engine.Core/System.h"

class LightUpdateSystem final : public System
{
public:
    void Tick(World& world, float) override
    {
        auto render = BenchmarkEngine::GetLocator().GetModule<RenderModule>();
        if (world.GetECS().Get<CameraComponent>(world.ActiveCamera)._CBufferIndex !=
            render->GetRenderPipelineCommonData()->ActiveCameraCBufferIndex) return;
        render->SubmitSky(world);
        auto lighting = CollectWorldLights(world.GetECS(), render->GetShadowQuality().LocalResolution != 0);
        render->SubmitLights(std::move(lighting.Local), lighting.Main);
    }
};

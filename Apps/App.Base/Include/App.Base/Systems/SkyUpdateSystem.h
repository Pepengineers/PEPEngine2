#pragma once
#include "App.Base/ECS/World.h"
#include "App.Base/Modules/RenderModule.h"
#include "Engine.Core/BenchmarkEngine.h"
#include "Engine.Core/System.h"
class SkyUpdateSystem final : public System
{
public:
    void Tick(World& world,float) override
    {
        auto render=BenchmarkEngine::GetLocator().GetModule<RenderModule>();
        auto& ecs=world.GetECS();
        if(ecs.Get<CameraComponent>(world.ActiveCamera)._CBufferIndex!=render->GetRenderPipelineCommonData()->ActiveCameraCBufferIndex) return;
        GDX12SkyDesc sky; sky.World=world.GetRuntimeId();
        ecs.ForEach<SkyBoxComponent>([&](Entity entity,SkyBoxComponent& box) { if(box.Enabled) sky.Boxes.push_back({entity,box}); });
        ecs.ForEach<SkyAtmosphereComponent>([&](Entity entity,SkyAtmosphereComponent& atmosphere) { if(atmosphere.Enabled) sky.Atmospheres.push_back({entity,atmosphere}); });
        render->SubmitSky(std::move(sky));
    }
};

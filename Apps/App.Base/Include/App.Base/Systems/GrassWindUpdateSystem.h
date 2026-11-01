#pragma once
#include "App.Base/ECS/World.h"
#include "App.Base/Modules/RenderModule.h"
#include "Engine.Core/BenchmarkEngine.h"
#include "Engine.Core/System.h"
#include <cmath>

class GrassWindUpdateSystem final : public System
{
public:
    void Tick(World& world,float dt) override
    {
        auto render=BenchmarkEngine::GetLocator().GetModule<RenderModule>(); auto& ecs=world.GetECS();
        if(!ecs.Has<CameraComponent>(world.ActiveCamera)||ecs.Get<CameraComponent>(world.ActiveCamera)._CBufferIndex!=render->GetRenderPipelineCommonData()->ActiveCameraCBufferIndex) return;
        GDX12GrassWindDesc submission; submission.World=world.GetRuntimeId(); submission.DeltaTime=world.IsPaused()?0:dt*world.GetTimeScale();
        ecs.ForEach<TransformComponent,GrassRenderComponent>([&](Entity entity,TransformComponent&,GrassRenderComponent& grass)
        {
            if(!grass.Enabled) return;
            ValidateGrass(grass);
            if(!grass.Material||grass.Material->Type==MaterialType::Transparent) throw std::invalid_argument("Grass requires an opaque or masked PBR material");
            GDX12GrassDesc desc; desc.Entity=entity; desc.Settings=grass;
            const auto& transform=render->GetTransformGPUData(entity); desc.World=transform.World; desc.TransformIndex=transform.CBufferIndex;
            desc.MaterialIndex=grass.Material->_CBufferIndex; submission.Grass.push_back(std::move(desc));
        });
        ecs.ForEach<TransformComponent,WindComponent>([&](Entity entity,TransformComponent& transform,WindComponent& wind)
        {
            if(!wind.Enabled) return; ValidateWind(wind);
            GDX12WindDesc desc; desc.Entity=entity; desc.Settings=wind; desc.Center=transform.Location;
            desc.Settings.DirectionDegrees=std::remainder(wind.DirectionDegrees+transform.Rotation.y,360.0f);
            const float scale=(std::max)(std::abs(transform.Scale.x),std::abs(transform.Scale.z));
            desc.Settings.HalfExtent*=scale; submission.Winds.push_back(desc);
        });
        render->SubmitGrassWind(std::move(submission));
    }
};

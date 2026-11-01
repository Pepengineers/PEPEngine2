#pragma once

#include "BistroProps.h"
#include "Engine.Core/BenchmarkEngine.h"
#include "App.Base/Modules/SceneManagerModule.h"
#include "App.Base/Modules/RenderModule.h"

inline void ConfigureBistroProps(SceneManagerModule& scenes)
{
    scenes.OnWorldCreated.AddListener([](World& world)
    {
        world.GetECS().RegisterComponent<FlyingPropComponent>();
        world.SceneMeshFactory = [&world](const std::string& name, const StaticMeshRenderComponent& source)
        {
            if (name != "Interior_interior") return false;
            auto& render = *BenchmarkEngine::GetLocator().GetModule<RenderModule>();
            const auto* mesh = render.GetPrimaryGPUMesh(source.MeshHandler);
            const auto partition = PartitionBistroProps(*mesh->CPUMesh);
            render.ReserveTransformCapacity(static_cast<UINT>(partition.Props.size()) + 2);
            CreateBistroPropEntities(world, name, source, partition);
            return true;
        };
    });
}

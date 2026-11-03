#pragma once

#include "../src/BistroWater.h"
#include <stdexcept>

// Drive the actual App-owned scene so renderer entity IDs remain in their owning world.
template<class Frame>
inline void CheckBistroWaterCounts(World& world, Frame&& frame)
{
    const auto require = [](bool value, const char* message)
    {
        if (!value) throw std::runtime_error(message);
    };
    auto& render = *BenchmarkEngine::GetLocator().GetModule<RenderModule>();
    auto& ecs = world.GetECS();
    require(BistroWater::Count.GetValue() == 1, "Bistro must default to one water basin");
    require(!BistroWater::Count.SetFromString("-1") && !BistroWater::Count.SetFromString("9") &&
        !BistroWater::Count.SetFromString("1.5"), "Invalid Bistro water count accepted");
    const size_t staticCount = ecs.GetPool<StaticMeshRenderComponent>().Size();
    require(ecs.GetPool<FluidRenderComponent>().Size() == 1, "Default Bistro water count mismatch");
    Entity first = InvalidEntity;
    std::shared_ptr<const std::vector<GDX12FluidParticle>> seed;
    ecs.ForEach<FluidRenderComponent, TransformComponent>([&](Entity entity, const FluidRenderComponent& fluid,
        const TransformComponent& transform)
    {
        first = entity;
        seed = fluid.GetParticles();
        const auto& settings = fluid.GetSettings();
        ValidateFluidSettings(settings);
        require(seed && seed->size() == 512 && settings.SimulationEnabled && settings.RenderEnabled,
            "Bistro water must contain 512 live solver/render particles");
        require(transform.Location == Vector3(-410,41.3f,-100), "Bistro water must sit outside the doorway");
        for (const auto& particle : *seed)
            require(std::abs(particle.Position.x) + settings.ParticleRadiusCm < settings.ContainerHalfExtentCm.x &&
                std::abs(particle.Position.y) + settings.ParticleRadiusCm < settings.ContainerHalfExtentCm.y &&
                std::abs(particle.Position.z) + settings.ParticleRadiusCm < settings.ContainerHalfExtentCm.z,
                "Bistro water seed must fit inside its container");
    });
    Engine::Core::MeshHandle sharedMesh;
    GDX12Material* sharedMaterial = nullptr;
    ecs.ForEach<StaticMeshRenderComponent>([&](Entity, const StaticMeshRenderComponent& basin)
    {
        const auto* mesh = render.GetPrimaryGPUMesh(basin.MeshHandler);
        if (!mesh || mesh->CPUMesh->GetSubMesh(0).SourceNodeName != "BistroWaterBasin") return;
        sharedMesh = basin.MeshHandler;
        require(basin.Materials.size() == 1 && basin.Materials[0] && basin.Materials[0]->Diffuse,
            "Bistro water basin material missing");
        sharedMaterial = basin.Materials[0];
        require(mesh && mesh->CPUMesh->GetSubMesh(0).GetVertexCount() == 120 &&
            mesh->CPUMesh->GetSubMesh(0).GetIndexCount() == 180 && !basin._CBufferIndices.empty(),
            "Bistro basin must upload five boxes and register an indirect instance");
        const auto* commands = render.GetPrimaryIndirectCommandsCache();
        require(commands && basin._CBufferIndices.size() == 1, "Bistro basin indirect template missing");
    });
    require(sharedMesh.IsValid(), "Bistro basin entity missing");
    world.SetPaused(true);
    frame();
    require(ecs.IsAlive(first) && ecs.Get<FluidRenderComponent>(first).GetParticles() == seed,
        "Paused water must keep its seed and entity");
    world.SetPaused(false);
    require(BistroWater::Count.SetFromString("2"), "Bistro water growth rejected");
    frame();
    require(ecs.GetPool<FluidRenderComponent>().Size() == 2 &&
        ecs.GetPool<StaticMeshRenderComponent>().Size() == staticCount + 1 && ecs.IsAlive(first),
        "Bistro water must grow without replacing the first basin");
    ecs.ForEach<FluidRenderComponent>([&](Entity, const FluidRenderComponent& fluid)
    { require(fluid.GetParticles() == seed, "Bistro basins must share their immutable seed"); });
    ecs.ForEach<StaticMeshRenderComponent>([&](Entity, const StaticMeshRenderComponent& basin)
    {
        if (basin.MeshHandler == sharedMesh)
            require(basin.Materials[0] == sharedMaterial, "Bistro basins must share their material");
    });
    require(BistroWater::Count.SetFromString("1"), "Bistro water shrink rejected");
    frame();
    require(ecs.GetPool<FluidRenderComponent>().Size() == 1 &&
        ecs.GetPool<StaticMeshRenderComponent>().Size() == staticCount && ecs.IsAlive(first),
        "Bistro water must shrink without replacing its first basin");
    require(BistroWater::Count.SetFromString("0"), "Bistro water disable rejected");
    frame();
    require(ecs.GetPool<FluidRenderComponent>().Size() == 0 &&
        ecs.GetPool<StaticMeshRenderComponent>().Size() == staticCount - 1,
        "Disabled Bistro water leaked water/basin entities");
    require(BistroWater::Count.SetFromString("1"), "Bistro water restore rejected");
    frame();
    require(ecs.GetPool<FluidRenderComponent>().Size() == 1 &&
        ecs.GetPool<StaticMeshRenderComponent>().Size() == staticCount, "Bistro water must restore after disabling");
}

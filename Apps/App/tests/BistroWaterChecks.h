#pragma once

#include "../src/BistroWater.h"
#include <stdexcept>

// Drive the actual App-owned scene so renderer entity IDs remain in their owning world.
template<class Frame>
inline void CheckBistroWaterCounts(World& world, Frame&& frame)
{
    RegisterBistroWaterComponents(world);
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
        require(seed && seed->size() >= 40000 && seed->size() <= 60000 && settings.SimulationEnabled && settings.RenderEnabled,
            "Bistro water must contain approximately 50000 live MGPU particles");
        require(settings.NearPressureStiffnessCm2PerSecond2 > 0 && settings.PredictionSeconds == 1.0f / 120 &&
            settings.MaxSubsteps == 3 && settings.MaxStepSeconds == 1.0f / 180 &&
            settings.ViscosityCm2PerSecond == 0 && settings.Restitution == 0.95f &&
            settings.AccelerationCmPerSecond2 == Vector3(0,-9.81f * 20,0), "Bistro must use the MGPU fluid preset");
        require(transform.Location == BistroWater::Center && transform.Rotation.y == BistroWater::YawDegrees &&
            settings.ContainerHalfExtentCm == BistroWater::HalfExtent,
            "Bistro water must span the side street beside the hedge");
        float lowest = 0, highest = 0;
        for (const auto& particle : *seed)
        {
            require(particle.Velocity == Vector3::Zero, "MGPU volume must spawn at rest");
            lowest = (std::min)(lowest, particle.Position.y);
            highest = (std::max)(highest, particle.Position.y);
            require(std::abs(particle.Position.x) + settings.ParticleRadiusCm < settings.ContainerHalfExtentCm.x &&
                std::abs(particle.Position.y) + settings.ParticleRadiusCm < settings.ContainerHalfExtentCm.y &&
                std::abs(particle.Position.z) + settings.ParticleRadiusCm < settings.ContainerHalfExtentCm.z,
                "Bistro water seed must fit inside its container");
        }
        require(highest - lowest > BistroWater::HalfExtent.y, "Water must spawn as a volume instead of two flat layers");
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
    const auto pausedAcceleration = ecs.Get<FluidRenderComponent>(first).GetSettings().AccelerationCmPerSecond2;
    const auto resetRevision = ecs.Get<FluidRenderComponent>(first).GetResetRevision();
    const auto settingsRevision = ecs.Get<FluidRenderComponent>(first).GetSettingsRevision();
    frame();
    require(ecs.IsAlive(first) && ecs.Get<FluidRenderComponent>(first).GetParticles() == seed &&
        ecs.Get<FluidRenderComponent>(first).GetSettings().AccelerationCmPerSecond2 == pausedAcceleration,
        "Paused water must keep its seed and entity");
    world.SetPaused(false);
    frame();
    require(ecs.Get<FluidRenderComponent>(first).GetSettings().AccelerationCmPerSecond2 == pausedAcceleration &&
        ecs.Get<FluidRenderComponent>(first).GetResetRevision() == resetRevision &&
        ecs.Get<FluidRenderComponent>(first).GetSettingsRevision() == settingsRevision &&
        ecs.Get<FluidRenderComponent>(first).GetParticles() == seed,
        "MGPU motion must keep gravity and the live seed without artificial periodic forces");
    auto& live = ecs.Get<FluidRenderComponent>(first);
    auto invalidSettings = live.GetSettings();
    invalidSettings.NearPressureStiffnessCm2PerSecond2 = -1;
    bool rejected = false;
    try { live.SetSettings(invalidSettings); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected && live.GetSettingsRevision() == settingsRevision,
        "Invalid MGPU settings must not change the live solver");
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

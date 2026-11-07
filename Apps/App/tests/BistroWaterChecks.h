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
    require(BistroWater::WaveSpeed.GetValue() == 6 && !BistroWater::WaveSpeed.SetConfigFromString("-1") &&
        !BistroWater::WaveSpeed.SetConfigFromString("13") && !BistroWater::WaveSpeed.SetConfigFromString("nan"),
        "Initial wave strength must have a finite, validated startup control");
    require(BistroWater::WaveAcceleration.GetValue() == 4 && BistroWater::WavePeriod.GetValue() == 6 &&
        !BistroWater::WaveAcceleration.SetConfigFromString("nan") && !BistroWater::WavePeriod.SetConfigFromString("0"),
        "Periodic wave controls must reject invalid acceleration and period");
    require(BistroWater::ParticleCount.GetValue() == 100000 && BistroWater::ParticleRadius.GetValue() == .15f &&
        !BistroWater::ParticleCount.SetConfigFromString("0") && !BistroWater::ParticleRadius.SetConfigFromString("0.3"),
        "Water volume and particle radius must have validated startup controls");
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
        require(seed && seed->size() >= 80000 && seed->size() <= 120000 && seed->size() % 2 == 0 && settings.SimulationEnabled && settings.RenderEnabled,
            "Bistro water must contain approximately 100000 mirrored MGPU particles");
        require(settings.MGPUReference && settings.ParticleMassKg == 1 && settings.RestDensityKgPerCm3 == 630 &&
            settings.SmoothingRadiusCm == 0.2f && settings.PressureStiffnessCm2PerSecond2 == 288 &&
            settings.NearPressureStiffnessCm2PerSecond2 == 2.15f && settings.PredictionSeconds == 1.0f / 120 &&
            settings.MaxSubsteps == 3 && settings.MaxStepSeconds == 1.0f / 180 &&
            settings.ViscosityCm2PerSecond == 0 && settings.Restitution == 0.95f &&
            settings.AccelerationCmPerSecond2 == Vector3(0,-9.81f,0), "Bistro must use the MGPU fluid preset");
        require(settings.WaveAcceleration == 4 && settings.WavePeriodSeconds == 6 && settings.ParticleRadiusCm == 0.15f,
            "Bistro must sustain waves with a periodic force along the narrow-to-narrow axis");
        require(transform.Location == BistroWater::Center && transform.Rotation.y == BistroWater::YawDegrees &&
            transform.Scale == BistroWater::ReferenceScale &&
            settings.ContainerHalfExtentCm * transform.Scale == BistroWater::HalfExtent,
            "Bistro water must span the side street beside the hedge");
        const auto extent = settings.ContainerHalfExtentCm;
        const float meanDepth = float(seed->size()) / (settings.RestDensityKgPerCm3 * 4 * extent.x * extent.z);
        require(meanDepth >= 3 * settings.SmoothingRadiusCm,
            "Reference bounds must retain a fluid layer at least three smoothing radii deep across the floor");
        float lowest = 0, highest = 0;
        for (const auto& particle : *seed)
        {
            const float direction = particle.Position.x < 0 ? -1.0f : 1.0f;
            require(particle.Velocity == Vector3(direction * BistroWater::WaveSpeed.GetValue(), 0, 0),
                "Both halves of the central volume must initially flow toward opposite narrow ends");
            require(std::abs(particle.Position.x) <= extent.x * .5f + .101f,
                "Water must fall from the center, leaving room at both ends");
            lowest = (std::min)(lowest, particle.Position.y);
            highest = (std::max)(highest, particle.Position.y);
            require(std::abs(particle.Position.x) < 5.3f && std::abs(particle.Position.z) < 10.1f &&
                std::abs(particle.Position.y - extent.y * 0.1f) <= extent.y * 0.9f + 0.101f,
                "MGPU seed must use particle-driven width/depth, 90% bounds and original jitter/offset");
        }
        for (size_t i = 0; i < seed->size(); i += 2)
        {
            const auto& left = (*seed)[i]; const auto& right = (*seed)[i+1];
            require(left.Position.x <= 0 && right.Position.x >= 0 &&
                left.Position.x == -right.Position.x && left.Position.y == right.Position.y && left.Position.z == right.Position.z &&
                left.Velocity == -right.Velocity,
                "The central spawn must contain exactly mirrored pairs with zero net horizontal momentum");
        }
        require(highest - lowest > settings.ContainerHalfExtentCm.y, "Water must spawn as a volume instead of two flat layers");
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

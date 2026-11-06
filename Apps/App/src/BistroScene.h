#pragma once

#include "BistroProps.h"
#include "Engine.Core/BenchmarkEngine.h"
#include "App.Base/Modules/SceneManagerModule.h"
#include "App.Base/Modules/RenderModule.h"
#include "Engine.Core/AssetManager.h"
#include "Common/ConsoleVariables.h"

namespace BistroParticles
{
    inline AutoConsoleVariable<int> Count(L"App.Particles.Count", -1,
        L"Total live Bistro particles: -1 uses CountScale, 0 disables, maximum 4194304",
        EConsoleVariableFlags::Default, [](int value) { return value >= -1 && value <= 4194304; });
    inline AutoConsoleVariable<double> CountScale(L"App.Particles.CountScale", 1.0,
        L"Multiply authored Bistro particle counts; <=0 uses 1, maximum 4194304/250000",
        EConsoleVariableFlags::Default, [](double value) { return value <= 4194304.0 / 250000; });
    inline AutoConsoleVariable<ParticleSimulation> Simulation(L"App.Particles.Simulation", ParticleSimulation::AnalyticDrag,
        L"0 analytic drag + trapezoidal position (authored default), 1 MGPU semi-implicit Euler");
    inline AutoConsoleVariable<bool> VolumeEnabled(L"App.Particles.Volume.Enabled", false,
        L"Override unbound emitters with box spawn and a reflecting emitter-local simulation volume; Dust/Steam use App.Bounds");
    inline AutoConsoleVariable<float> VolumeHalfExtentX(L"App.Particles.Volume.HalfExtentX", 100.0f,
        L"Particle box half-size X in emitter-local cm, 0.01..1000000",
        EConsoleVariableFlags::Default, [](float value) { return value >= 0.01f && value <= 1000000; });
    inline AutoConsoleVariable<float> VolumeHalfExtentY(L"App.Particles.Volume.HalfExtentY", 100.0f,
        L"Particle box half-size Y in emitter-local cm, 0.01..1000000",
        EConsoleVariableFlags::Default, [](float value) { return value >= 0.01f && value <= 1000000; });
    inline AutoConsoleVariable<float> VolumeHalfExtentZ(L"App.Particles.Volume.HalfExtentZ", 100.0f,
        L"Particle box half-size Z in emitter-local cm, 0.01..1000000",
        EConsoleVariableFlags::Default, [](float value) { return value >= 0.01f && value <= 1000000; });

    inline UINT GetBudget()
    {
        const int count = Count.GetValue();
        if (count >= 0) return static_cast<UINT>(count);
        const double scale = CountScale.GetValue();
        return static_cast<UINT>(std::round(250000 * (scale > 0 ? scale : 1)));
    }

    struct EffectComponent : ComponentTag
    {
        UINT BudgetBegin = 0;
        UINT BudgetEnd = 0;
        ParticleShape AuthoredShape = ParticleShape::Point;
        Vector3 AuthoredExtents;
        bool LayoutVolume = false;
    };

    inline void Update(World& world)
    {
        const auto total = static_cast<uint64_t>(GetBudget());
        const auto simulation = Simulation.GetValue();
        const bool bounded = VolumeEnabled.GetValue();
        const Vector3 halfExtents(VolumeHalfExtentX.GetValue(), VolumeHalfExtentY.GetValue(), VolumeHalfExtentZ.GetValue());
        world.GetECS().ForEach<EffectComponent, ParticleEmitterComponent>(
            [&](Entity, const EffectComponent& effect, ParticleEmitterComponent& emitter)
            {
                const UINT count = static_cast<UINT>(total * effect.BudgetEnd / 16 - total * effect.BudgetBegin / 16);
                const auto& current = emitter.GetSettings();
                auto settings = current;
                const bool useVolume = bounded || effect.LayoutVolume;
                const auto volumeExtents = effect.LayoutVolume ? effect.AuthoredExtents : halfExtents;
                settings.MaxParticles = (std::max)(1u, count);
                settings.Simulation = simulation;
                settings.VolumeEnabled = useVolume;
                settings.VolumeHalfExtents = volumeExtents;
                settings.Shape = useVolume ? ParticleShape::Box : effect.AuthoredShape;
                settings.ShapeExtents = useVolume ? volumeExtents : effect.AuthoredExtents;
                const bool reset = current.MaxParticles != settings.MaxParticles || emitter.IsEnabled() != (count != 0) ||
                    current.VolumeEnabled != useVolume || current.Shape != settings.Shape || current.ShapeExtents != settings.ShapeExtents ||
                    (useVolume && current.VolumeHalfExtents != volumeExtents);
                if (reset || current.Simulation != simulation || current.VolumeHalfExtents != volumeExtents)
                {
                    emitter.SetSettings(settings);
                    if (reset) emitter.Reset();
                    emitter.SetEnabled(count != 0);
                }
                // GPU spawn accepts only dead slots, keeping the requested live count without CPU readback.
                if (count && !world.IsPaused() && !emitter.IsPaused()) emitter.Emit(count);
            });
    }

    inline void Create(World& world, GPUTexture* atlas)
    {
        auto& ecs = world.GetECS();
        ecs.RegisterComponent<EffectComponent>();
        UINT weightSum = 0;
        const auto create = [&](const char* name, Vector3 position, UINT weight, GDX12ParticleEmitterSettings settings)
        {
            auto entity = ecs.CreateEntity();
            entity.AddComponent<NameComponent>(name);
            entity.AddComponent<TransformComponent>(position);
            auto& effect = entity.AddComponent<EffectComponent>();
            effect.BudgetBegin = weightSum;
            effect.BudgetEnd = weightSum += weight;
            settings.Seed = weightSum * 7919;
            settings.EmissionRate = 0;
            settings.Substeps = settings.DepthCollision ? 4 : 1;
            settings.EdgeSamples = 1;
            settings.MinPixelSize = 0;
            effect.AuthoredShape = settings.Shape;
            effect.AuthoredExtents = settings.ShapeExtents;
            entity.AddComponent<ParticleEmitterComponent>().SetSettings(settings);
        };

        GDX12ParticleEmitterSettings settings;
        settings.LifetimeMin = 2; settings.LifetimeMax = 3;
        settings.SpeedMin = 220; settings.SpeedMax = 320;
        settings.ConeAngleDegrees = 14;
        settings.Shape = ParticleShape::Sphere; settings.ShapeExtents = Vector3(8, 2, 8);
        settings.Acceleration = Vector3(0, -260, 0);
        settings.SizeStart = 7.5f; settings.SizeEnd = 2;
        settings.ColorStart = Vector4(4, 2.2f, 0.5f, 0.3f); settings.ColorEnd = Vector4(1, 0.1f, 0.01f, 0);
        settings.Blend = ParticleBlend::Additive;
        settings.Texture = atlas; settings.AtlasColumns = settings.AtlasRows = 8;
        settings.SoftDistance = 2; settings.DepthCollision = true;
        create("BistroEntranceFountain", {-170, 45, -170}, 4, settings);

        settings.SpeedMin = 180; settings.SpeedMax = 290;
        settings.ConeAngleDegrees = 20;
        settings.ColorStart = Vector4(0.3f, 1.5f, 4, 0.3f); settings.ColorEnd = Vector4(0.05f, 0.2f, 1, 0);
        create("BistroBlueFountain", {-70, 45, 65}, 3, settings);

        settings = {};
        settings.LifetimeMin = 3; settings.LifetimeMax = 6;
        settings.SpeedMin = 15; settings.SpeedMax = 45;
        settings.Shape = ParticleShape::Box; settings.ShapeExtents = Vector3(100, 15, 70);
        settings.Acceleration = Vector3(0, 12, 0); settings.Wind = Vector3(10, 0, 5); settings.Drag = 0.5f;
        settings.SizeStart = 4; settings.SizeEnd = 1;
        settings.ColorStart = Vector4(3, 0.6f, 0.05f, 0.3f); settings.ColorEnd = Vector4(1, 0.1f, 0.01f, 0);
        settings.Blend = ParticleBlend::Additive;
        create("BistroTerraceEmbers", {-480, 65, -430}, 2, settings);

        settings.LifetimeMin = 4; settings.LifetimeMax = 8;
        settings.SpeedMin = 5; settings.SpeedMax = 25; settings.ConeAngleDegrees = 180;
        settings.Shape = ParticleShape::Sphere; settings.ShapeExtents = Vector3(140, 50, 130);
        settings.Acceleration = Vector3::Zero; settings.Wind = Vector3(8, 0, 0);
        settings.SizeStart = 10; settings.SizeEnd = 6; settings.AngularVelocity = 1.2f;
        settings.Geometry = ParticleGeometry::Firefly;
        settings.Blend = ParticleBlend::WeightedAlpha;
        settings.ColorStart = Vector4(1, 1, 1, 0.9f); settings.ColorEnd = Vector4(1, 1, 1, 0);
        create("BistroPlanterFireflies", {-420, 110, 150}, 2, settings);

        settings = {};
        settings.LifetimeMin = 0.7f; settings.LifetimeMax = 1.8f;
        settings.SpeedMin = 80; settings.SpeedMax = 170; settings.ConeAngleDegrees = 180;
        settings.Shape = ParticleShape::Box; settings.ShapeExtents = Vector3(55, 10, 55);
        settings.Acceleration = Vector3(0, -50, 0);
        settings.SizeStart = 12; settings.SizeEnd = 25;
        settings.ColorStart = Vector4(3, 1, 0.35f, 0.04f); settings.ColorEnd = Vector4(0.4f, 0.05f, 0.01f, 0);
        settings.Texture = atlas; settings.AtlasColumns = settings.AtlasRows = 8;
        settings.Blend = ParticleBlend::Additive; settings.DepthCollision = true;
        create("BistroStreetFirebursts", {-350, 90, -700}, 2, settings);

        settings = {};
        settings.LifetimeMin = 6; settings.LifetimeMax = 10;
        settings.SpeedMin = 2; settings.SpeedMax = 10; settings.ConeAngleDegrees = 180;
        settings.Shape = ParticleShape::Box; settings.ShapeExtents = Vector3(250, 100, 180);
        settings.Acceleration = Vector3::Zero; settings.Wind = Vector3(6, 0, 3); settings.Drag = 1;
        settings.SizeStart = 1.8f; settings.SizeEnd = 0.5f;
        settings.ColorStart = Vector4(0.8f, 0.7f, 0.5f, 0.12f); settings.ColorEnd = Vector4(0.8f, 0.7f, 0.5f, 0);
        create("BistroInteriorDust", {160, 180, -160}, 1, settings);

        settings.LifetimeMin = 2; settings.LifetimeMax = 5;
        settings.SpeedMin = 12; settings.SpeedMax = 35; settings.ConeAngleDegrees = 20;
        settings.ShapeExtents = Vector3(35, 1, 20);
        settings.Acceleration = Vector3(0, 2, 0); settings.Wind = Vector3(4, 0, 0); settings.Drag = 0.4f;
        settings.SizeStart = 4; settings.SizeEnd = 16; settings.SoftDistance = 12;
        settings.ColorStart = Vector4(0.7f, 0.8f, 0.9f, 0.015f); settings.ColorEnd = Vector4(0.7f, 0.8f, 0.9f, 0);
        create("BistroTableSteam", {550, 130, -350}, 1, settings);

        settings = {};
        settings.LifetimeMin = 3; settings.LifetimeMax = 7;
        settings.Direction = Vector3(0, -1, 0); settings.ConeAngleDegrees = 12;
        settings.SpeedMin = 20; settings.SpeedMax = 65;
        settings.Shape = ParticleShape::Box; settings.ShapeExtents = Vector3(180, 10, 140);
        settings.Acceleration = Vector3(0, -30, 0); settings.Wind = Vector3(25, 0, 8); settings.Drag = 0.4f;
        settings.SizeStart = 3; settings.SizeEnd = 1; settings.AngularVelocity = 3;
        settings.ColorStart = Vector4(2, 0.3f, 1, 0.3f); settings.ColorEnd = Vector4(0.5f, 0.05f, 0.2f, 0);
        settings.Blend = ParticleBlend::Additive; settings.DepthCollision = true;
        create("BistroSparkRain", {-650, 300, -120}, 1, settings);
        if (weightSum != 16) throw std::logic_error("Bistro particle weights must sum to 16");
        Update(world);
    }
}

inline void CreateBistroParticles(World& world)
{
    auto& render = *BenchmarkEngine::GetLocator().GetModule<RenderModule>();
    const auto* texture = Engine::Core::AssetManager::GetInstance().LoadTexture(
        std::filesystem::path(ASSETS_FOLDER) / "Textures/Particles/mgpu-fountain-atlas.png");
    if (!texture) throw std::runtime_error("Missing MGPU fountain atlas");
    auto* atlas = render.GetTextureByName("BistroFountainAtlas");
    if (!atlas) atlas = render.CreateTexture("BistroFountainAtlas", texture, Engine::Core::ETextureType::Color);
    if (!atlas) throw std::runtime_error("Cannot create Bistro fountain atlas");
    render.WaitForUploads();

    BistroParticles::Create(world, atlas);
}

inline void ConfigureBistroProps(SceneManagerModule& scenes, JobSystem& jobs)
{
    scenes.OnWorldCreated.AddListener([&jobs](World& world)
    {
        auto waves = std::make_shared<BistroWaves::Controller>();
        world.BeforePhysicsStep = [&world, waves](float dt)
        { waves->Tick(world, *BenchmarkEngine::GetLocator().GetModule<PhysicsModule>(), dt); };
        world.SceneMeshFactory = [&world, &jobs, waves, configured = false](const std::string& name, const StaticMeshRenderComponent& source) mutable
        {
            if (name != "Interior_interior" && name != "Exterior_exterior") return false;
            auto& physics = *BenchmarkEngine::GetLocator().GetModule<PhysicsModule>();
            if (!configured) { physics.ConfigureWorld(world, BistroPhysics::Settings()); configured = true; }
            auto& render = *BenchmarkEngine::GetLocator().GetModule<RenderModule>();
            const auto* mesh = render.GetPrimaryGPUMesh(source.MeshHandler);
            const auto partition = PartitionBistroProps(*mesh->CPUMesh, name == "Interior_interior");
            const auto count = BistroPhysics::GetCount(partition, BistroPhysics::CountScale.GetValue());
            render.ReserveTransformCapacity(static_cast<UINT>(count) + 9);
            size_t instances = partition.StaticSubMeshes.size();
            for (const auto& prop : partition.Props) instances += prop.SubMeshIndices.size();
            for (size_t i=0; i<count-partition.Props.size(); ++i)
                instances += partition.Props[partition.Tableware[i%partition.Tableware.size()]].SubMeshIndices.size();
            render.ReserveInstanceCapacity(static_cast<UINT>(instances));
            CreateBistroPropEntities(world, name, source, *mesh->CPUMesh, partition, jobs, &physics, waves.get());
            if (name == "Interior_interior") CreateBistroParticles(world);
            return true;
        };
    });
}

#pragma once

#include "../src/BistroScene.h"
#include <stdexcept>

inline void CheckBistroParticleControls()
{
    const auto require = [](bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    };
    using namespace BistroParticles;
    require(Count.GetValue() == -1 && CountScale.GetValue() == 2 && !VolumeEnabled.GetValue() &&
        Simulation.GetValue() == ParticleSimulation::SemiImplicitEuler,
        "Particle controls must preserve authored defaults");
    require(!Count.SetFromString("-2") && !Count.SetFromString("4194305") && !Count.SetFromString("1.5") &&
        !Simulation.SetFromString("2") && !VolumeHalfExtentX.SetFromString("0") &&
        !VolumeHalfExtentY.SetFromString("nan") && !VolumeHalfExtentZ.SetFromString("1000001"),
        "Particle controls accepted invalid input");
    World world(WorldDesc{});
    Create(world, nullptr);
    unsigned fountains = 0;
    world.GetECS().ForEach<NameComponent, ParticleEmitterComponent>([&](Entity, const NameComponent& name,
        const ParticleEmitterComponent& emitter)
    {
        const auto& settings = emitter.GetSettings();
        const bool fountain = name.value == "BistroEntranceFountain" || name.value == "BistroBlueFountain";
        require(settings.CollisionBoxEnabled == fountain, "Only the entrance fountains must collide with the door");
        if (!fountain) return;
        ++fountains;
        require(settings.DepthCollision && settings.Substeps >= 4 && settings.CollisionRestitution > 0 &&
            settings.CollisionFriction == 0.2f &&
            settings.CollisionRadius >= (std::max)(settings.SizeStart, settings.SizeEnd) *
                (1 + settings.SizeVariation) * settings.RenderScale * 0.707107f,
            "Fountains must bounce against scene depth without their sprite corners penetrating surfaces");
    });
    require(fountains == 2, "Both entrance fountains must retain their collision settings");
    const auto budget = [&]
    {
        UINT result = 0;
        world.GetECS().ForEach<ParticleEmitterComponent>([&](Entity, const ParticleEmitterComponent& emitter)
        {
            if (emitter.IsEnabled()) result += emitter.GetSettings().MaxParticles;
        });
        return result;
    };
    require(budget() == 500000, "Bistro must double the authored particle budget");
    world.GetECS().ForEach<NameComponent, TransformComponent, ParticleEmitterComponent>(
        [&](Entity, const NameComponent& name, const TransformComponent& transform, const ParticleEmitterComponent& emitter)
        {
            if (name.value != "BistroSparkRain") return;
            const auto& settings = emitter.GetSettings();
            require(transform.Location == BistroWater::Center + Vector3(0,300,0) &&
                transform.Rotation.y == BistroWater::YawDegrees &&
                settings.ShapeExtents == Vector3(BistroWater::HalfExtent.x,10,BistroWater::HalfExtent.z) &&
                settings.Simulation == ParticleSimulation::SemiImplicitEuler &&
                settings.ConeAngleDegrees == 0 && settings.Wind == Vector3::Zero,
                "Rain must fall over the pool's exact rotated footprint using MGPU integration");
        });
    require(CountScale.SetFromString("0.5"), "Particle count scale rejected");
    Update(world);
    require(budget() == 125000, "Inherited particle count scale changed");
    for (const int requested : {1, 7, 17, 4194304})
    {
        require(Count.SetValue(requested), "Absolute particle budget rejected");
        Update(world);
        require(GetBudget() == static_cast<UINT>(requested) && budget() == static_cast<UINT>(requested),
            "Absolute count must override the scale exactly, including tiny and maximum budgets");
        world.GetECS().ForEach<ParticleEmitterComponent>([&](Entity, const ParticleEmitterComponent& emitter)
        { require(emitter.GetSettings().MaxParticles <= 1048576, "Absolute count exceeded an individual pool limit"); });
    }
    require(Count.SetFromString("0"), "Particle disable rejected");
    Update(world);
    require(budget() == 0, "Zero absolute count must disable every emitter");
    world.GetECS().ForEach<ParticleEmitterComponent>([&](Entity, const ParticleEmitterComponent& emitter)
    { require(!emitter.IsEnabled(), "Zero budget retained an enabled emitter"); });

    require(Count.SetFromString("-1") && CountScale.SetFromString("1") &&
        VolumeEnabled.SetFromString("true") && VolumeHalfExtentX.SetFromString("20") &&
        VolumeHalfExtentY.SetFromString("30") && VolumeHalfExtentZ.SetFromString("40") &&
        Simulation.SetFromString("1"), "Particle controls update rejected");
    Update(world);
    uint64_t resetSum = 0;
    world.GetECS().ForEach<ParticleEmitterComponent>([&](Entity, const ParticleEmitterComponent& emitter)
    {
        const auto& settings = emitter.GetSettings();
        const auto extents = emitter.GetSettings().Geometry == ParticleGeometry::Firefly ?
            Vector3(BistroArch::SwarmBounds.Extents) - Vector3(10 * (1 + settings.SizeVariation) * settings.RenderScale * 0.707107f) : Vector3(20,30,40);
        require(settings.Shape == ParticleShape::Box && settings.ShapeExtents == extents &&
            settings.VolumeEnabled && settings.VolumeHalfExtents == extents &&
            settings.Simulation == ParticleSimulation::SemiImplicitEuler, "Particle controls did not reach each emitter");
        resetSum += emitter.GetResetRevision();
    });
    Update(world);
    uint64_t unchangedResetSum = 0;
    world.GetECS().ForEach<ParticleEmitterComponent>([&](Entity, const ParticleEmitterComponent& emitter)
    { unchangedResetSum += emitter.GetResetRevision(); });
    require(resetSum == unchangedResetSum, "Unchanged particle controls reset the live pools");
    require(Simulation.SetFromString("0"), "Particle integration switch rejected");
    Update(world);
    unchangedResetSum = 0;
    world.GetECS().ForEach<ParticleEmitterComponent>([&](Entity, const ParticleEmitterComponent& emitter)
    { unchangedResetSum += emitter.GetResetRevision(); });
    require(resetSum == unchangedResetSum, "Changing the integrator unnecessarily reset particles");
    require(VolumeEnabled.SetFromString("false"), "Authored spawn restoration rejected");
    Update(world);
    world.GetECS().ForEach<EffectComponent, ParticleEmitterComponent>([&](Entity, const EffectComponent& effect,
        const ParticleEmitterComponent& emitter)
    {
        require(emitter.GetSettings().Shape == effect.AuthoredShape && emitter.GetSettings().ShapeExtents == effect.AuthoredExtents &&
            emitter.GetSettings().VolumeEnabled == effect.LayoutVolume, "Disabling the volume must preserve authored confinement");
    });
    require(CountScale.SetFromString("2") && Simulation.SetFromString("1"), "Particle defaults restore failed");
    Update(world);
    require(budget() == 500000 && VolumeHalfExtentX.SetFromString("100") &&
        VolumeHalfExtentY.SetFromString("100") && VolumeHalfExtentZ.SetFromString("100"), "Particle defaults restore failed");
}

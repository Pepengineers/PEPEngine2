#pragma once

#include "../src/BistroGrass.h"
#include <stdexcept>

// Run after the App initializes its renderer; the isolated world shares its owned material.
inline void CheckBistroGrassCounts()
{
    const auto require = [](bool value, const char* message)
    {
        if (!value) throw std::runtime_error(message);
    };
    World world(WorldDesc{});
    BistroGrass::Scene scene;
    auto& ecs = world.GetECS();
    require(BistroGrass::Count.GetValue() == 1, "Bistro must default to one grass patch");
    require(!BistroGrass::Count.SetFromString("-1") && !BistroGrass::Count.SetFromString("17") &&
        !BistroGrass::Count.SetFromString("1.5"), "Invalid Bistro grass count accepted");
    scene.Update(world, 0);
    require(ecs.GetPool<GrassRenderComponent>().Size() == 1 &&
        ecs.GetPool<WindFieldComponent>().Size() == 1, "Default Bistro grass/wind count mismatch");
    Entity first = InvalidEntity;
    GDX12Material* sharedMaterial = nullptr;
    ecs.ForEach<GrassRenderComponent, TransformComponent>([&](Entity entity, const GrassRenderComponent& grass,
        const TransformComponent& transform)
    {
        first = entity;
        sharedMaterial = grass.Material;
        require(IsValidGrassSettings(grass) && GrassBladeCount(ResolveGrassSettings(grass, 2)) == 59360,
            "Bistro grass settings or blade budget invalid");
        require(grass.Implementation == GDX12GrassImplementation::Ribbon && grass.Segments == 4 &&
            grass.HeightCm == 40 && grass.WidthCm == 3 && grass.MaxBendCm == 20,
            "Arch grass must use dense four-segment compute ribbons");
        require(grass.Material && grass.Material->Diffuse && grass.Material->Diffuse->PrimaryDeviceTexture &&
            grass.Material->Type == MaterialType::Opaque, "Bistro grass material is not ready for rendering");
        require(ecs.Has<WindFieldComponent>(grass.WindEntity) &&
            ecs.Get<WindFieldComponent>(grass.WindEntity).Implementation == GDX12WindImplementation::Analytic,
            "Bistro grass analytic wind link missing");
        require(transform.Location == BistroArch::GrassPosition && transform.Rotation.y == 69, "Bistro grass must fill the arch footprint");
        require(!grass.CaptureReflections && grass.CastShadows, "Bistro grass reflection/shadow policy changed");
    });
    world.SetPaused(true);
    scene.Update(world, 1);
    require(ecs.IsAlive(first), "Paused Bistro grass must preserve its entity");
    world.SetPaused(false);
    require(BistroGrass::Count.SetFromString("3"), "Bistro grass growth rejected");
    scene.Update(world, 0);
    require(ecs.GetPool<GrassRenderComponent>().Size() == 3 && ecs.GetPool<WindFieldComponent>().Size() == 3 &&
        ecs.IsAlive(first), "Bistro grass must grow without replacing existing patches");
    ecs.ForEach<GrassRenderComponent>([&](Entity, const GrassRenderComponent& grass)
    {
        require(grass.Material == sharedMaterial, "Bistro grass patches must share their material");
        require(ecs.Has<WindFieldComponent>(grass.WindEntity), "Bistro grass growth lost its wind owner");
    });
    require(BistroGrass::Implementation.GetEnumCount() == 2 && BistroGrass::Algorithm.GetEnumCount() == 2,
        "Grass and wind implementation controls must expose enum choices to the settings UI");
    const Entity firstWind = ecs.Get<GrassRenderComponent>(first).WindEntity;
    const UINT initialGrassRevision = ecs.Get<GrassRenderComponent>(first).ResetRevision;
    const UINT initialWindRevision = ecs.Get<WindFieldComponent>(firstWind).ResetRevision;
    scene.Update(world, 0);
    require(ecs.Get<GrassRenderComponent>(first).ResetRevision == initialGrassRevision &&
        ecs.Get<WindFieldComponent>(firstWind).ResetRevision == initialWindRevision,
        "Unchanged grass controls must not reset GPU history or the wind simulation");
    require(!BistroGrass::ExtentsXCm.SetFromString("0") && !BistroGrass::WidthCm.SetFromString("0") &&
        !BistroGrass::Density.SetFromString("100001") && !BistroGrass::MaxBlades.SetFromString("262145") &&
        !BistroGrass::Segments.SetFromString("9") && !BistroGrass::Algorithm.SetFromString("2") &&
        !BistroGrass::WindSpeed.SetFromString("501") && !BistroGrass::GridResolution.SetFromString("7") &&
        !BistroGrass::GustFrequencyHz.SetFromString("nan"), "Invalid grass/wind controls accepted");
    require(BistroGrass::ExtentsXCm.SetFromString("120") && BistroGrass::ExtentsZCm.SetFromString("90") &&
        BistroGrass::HeightCm.SetFromString("45") && BistroGrass::Density.SetFromString("100") &&
        BistroGrass::MaxBlades.SetFromString("10000") && BistroGrass::Segments.SetFromString("5") &&
        BistroGrass::Implementation.SetFromString("0") && BistroGrass::Algorithm.SetFromString("1") &&
        BistroGrass::WindAngleDegrees.SetFromString("90") && BistroGrass::WindSpeed.SetFromString("80") &&
        BistroGrass::GridResolution.SetFromString("32") && BistroGrass::PressureIterations.SetFromString("8"),
        "Grass/wind controls must accept supported runtime values");
    scene.Update(world, 0);
    ecs.ForEach<GrassRenderComponent>([&](Entity, const GrassRenderComponent& grass)
    {
        const auto& wind = ecs.Get<WindFieldComponent>(grass.WindEntity);
        const auto bounds = GrassLocalBounds(grass);
        require(grass.ExtentsCm == Vector2(120, 90) && grass.HeightCm == 45 && grass.Segments == 5 &&
            grass.Implementation == GDX12GrassImplementation::Ribbon && GrassBladeCount(grass) == 432,
            "Runtime grass controls must update every existing patch, including area-driven blade count");
        require(IsValidGrassSettings(grass) && IsValidWindSettings(wind) &&
            wind.ExtentsCm.x >= bounds.Extents.x && wind.ExtentsCm.y >= bounds.Extents.z &&
            wind.Implementation == GDX12WindImplementation::ProjectedGrid && wind.GridResolution == 32 &&
            wind.PressureIterations == 8 && wind.SpeedCmPerSecond == 80 && std::abs(wind.Direction.y - 1) < 0.0001f,
            "Runtime wind controls must update every field and cover changed grass bounds");
    });
    const UINT editedWindRevision = ecs.Get<WindFieldComponent>(firstWind).ResetRevision;
    require(BistroGrass::HeightCm.SetFromString("60") && BistroGrass::MaxBlades.SetFromString("50"),
        "Grass height/budget edits rejected");
    scene.Update(world, 0);
    require(GrassBladeCount(ecs.Get<GrassRenderComponent>(first)) == 50 &&
        ecs.Get<WindFieldComponent>(firstWind).ResetRevision == editedWindRevision,
        "Blade budget must cap density while height-only edits preserve the wind simulation");
    require(BistroGrass::Density.SetFromString("0"), "Zero grass density rejected");
    scene.Update(world, 0);
    require(GrassBladeCount(ecs.Get<GrassRenderComponent>(first)) == 0,
        "Zero density must remove drawn blades without removing the patch");
    require(BistroGrass::ExtentsXCm.SetFromString("200") && BistroGrass::ExtentsZCm.SetFromString("530") &&
        BistroGrass::HeightCm.SetFromString("40") && BistroGrass::Density.SetFromString("1400") &&
        BistroGrass::MaxBlades.SetFromString("65536") && BistroGrass::Segments.SetFromString("4") &&
        BistroGrass::Implementation.SetFromString("0") && BistroGrass::Algorithm.SetFromString("0") &&
        BistroGrass::WindAngleDegrees.SetFromString("67.5") && BistroGrass::WindSpeed.SetFromString("100") &&
        BistroGrass::GridResolution.SetFromString("0") && BistroGrass::PressureIterations.SetFromString("0"),
        "Grass/wind defaults restore rejected");
    scene.Update(world, 0);
    require(GrassBladeCount(ecs.Get<GrassRenderComponent>(first)) == 59360,
        "Restoring controls must preserve the authored default grass budget");
    require(BistroGrass::Count.SetFromString("1"), "Bistro grass reduction rejected");
    scene.Update(world, 0);
    require(ecs.GetPool<GrassRenderComponent>().Size() == 1 && ecs.GetPool<WindFieldComponent>().Size() == 1 &&
        ecs.IsAlive(first), "Bistro grass must shrink without replacing the first patch");
    require(BistroGrass::Count.SetFromString("0"), "Bistro grass disable rejected");
    scene.Update(world, 0);
    require(ecs.GetEntityCount() == 0, "Disabled Bistro grass leaked grass/wind entities");
    require(BistroGrass::Count.SetFromString("1"), "Bistro grass default restore rejected");
    scene.Update(world, 0);
    require(ecs.GetPool<GrassRenderComponent>().Size() == 1 && ecs.GetPool<WindFieldComponent>().Size() == 1,
        "Bistro grass must restore after being disabled");
}

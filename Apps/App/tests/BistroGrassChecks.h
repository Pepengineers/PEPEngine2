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
        require(IsValidGrassSettings(grass) && GrassBladeCount(ResolveGrassSettings(grass, 2)) == 1232,
            "Bistro grass settings or blade budget invalid");
        require(grass.Material && grass.Material->Diffuse && grass.Material->Diffuse->PrimaryDeviceTexture &&
            grass.Material->Type == MaterialType::Opaque, "Bistro grass material is not ready for rendering");
        require(ecs.Has<WindFieldComponent>(grass.WindEntity) &&
            ecs.Get<WindFieldComponent>(grass.WindEntity).Implementation == GDX12WindImplementation::Analytic,
            "Bistro grass analytic wind link missing");
        require(transform.Location == Vector3(-420, 23.3f, 150), "Bistro grass must sit beside the entrance");
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

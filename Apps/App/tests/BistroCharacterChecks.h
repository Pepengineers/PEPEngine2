#pragma once

#include "../src/BistroCharacters.h"
#include <stdexcept>

inline void CheckBistroCharacterCounts()
{
    const auto require = [](bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    };
    World world(WorldDesc{});
    BistroCharacters::Scene scene;
    auto& ecs = world.GetECS();
    require(BistroCharacters::NPCCount.GetValue() == 4 && BistroCharacters::FoxCount.GetValue() == 1,
        "Bistro must default to four humans and one fox");
    require(!BistroCharacters::NPCCount.SetFromString("-1") && !BistroCharacters::NPCCount.SetFromString("17") &&
        !BistroCharacters::NPCCount.SetFromString("1.5") && !BistroCharacters::FoxCount.SetFromString("9"),
        "Invalid Bistro character count accepted");
    scene.Update(world, 0);
    require(ecs.GetPool<SkeletalMeshRenderComponent>().Size() == 5, "Default Bistro character count mismatch");
    Engine::Core::SceneHandle sharedNPC;
    Entity firstNPC = InvalidEntity, firstFox = InvalidEntity;
    auto& assets = Engine::Core::AssetManager::GetInstance();
    ecs.ForEach<SkeletalMeshRenderComponent, NameComponent>([&](Entity entity,
        const SkeletalMeshRenderComponent& skeletal, const NameComponent& name)
    {
        const bool fox = name.value.find("BistroFox") == 0;
        const auto* source = assets.Scenes().GetScene(skeletal.GetScene());
        require(source && skeletal.GetClip() >= 0 && skeletal.IsLooping(), "Character scene/looping clip missing");
        require(source->GetClips().at(skeletal.GetClip()).Name == (fox ? "Walk" : "Take1"), "Wrong walking clip");
        require(source->EvaluatePose(skeletal.GetClip(), 0, true) != source->EvaluatePose(skeletal.GetClip(), 0.3, true),
            "Character walking pose must change");
        const auto& transform = ecs.Get<TransformComponent>(entity);
        require(transform.Scale.x > 0 && transform.Scale.y > 0 && transform.Scale.z > 0,
            "Character must use positive calibrated scale");
        require(std::abs(transform.Location.y - (fox ? 39.722f : 27.762f)) < 0.001f,
            "Characters must stand on the authored Bistro floor/pavement");
        if (fox) firstFox = entity;
        else
        {
            if (firstNPC == InvalidEntity) { firstNPC = entity; sharedNPC = skeletal.GetScene(); }
            require(skeletal.GetScene() == sharedNPC, "Human instances must share the imported scene asset");
        }
    });
    require(firstNPC != InvalidEntity && firstFox != InvalidEntity, "Both character populations are required");
    const auto npcStart = ecs.Get<TransformComponent>(firstNPC).Location;
    const auto foxStart = ecs.Get<TransformComponent>(firstFox).Location;
    scene.Update(world, 0.5f);
    require(ecs.Get<TransformComponent>(firstNPC).Location != npcStart &&
        ecs.Get<TransformComponent>(firstFox).Location != foxStart, "Both character populations must walk");
    const auto pausedNPC = ecs.Get<TransformComponent>(firstNPC).Location;
    const auto pausedFox = ecs.Get<TransformComponent>(firstFox).Location;
    world.SetPaused(true);
    scene.Update(world, 1);
    require(ecs.Get<TransformComponent>(firstNPC).Location == pausedNPC &&
        ecs.Get<TransformComponent>(firstFox).Location == pausedFox, "World pause must stop character movement");
    world.SetPaused(false);
    world.SetTimeScale(0);
    scene.Update(world, 1);
    require(ecs.Get<TransformComponent>(firstNPC).Location == pausedNPC, "Zero time scale must stop movement");
    world.SetTimeScale(1);
    require(BistroCharacters::NPCCount.SetFromString("2") && BistroCharacters::FoxCount.SetFromString("3"),
        "Character count change rejected");
    scene.Update(world, 0);
    require(ecs.GetPool<SkeletalMeshRenderComponent>().Size() == 5, "Changed character count mismatch");
    size_t humans = 0, foxes = 0;
    ecs.ForEach<NameComponent>([&](Entity, const NameComponent& name)
    {
        humans += name.value.find("BistroNPC") == 0;
        foxes += name.value.find("BistroFox") == 0;
    });
    require(humans == 2 && foxes == 3, "Character populations must reconcile independently");
    scene.Update(world, 30);
    ecs.ForEach<SkeletalMeshRenderComponent, NameComponent>([&](Entity entity,
        const SkeletalMeshRenderComponent&, const NameComponent& name)
    {
        const bool fox = name.value.find("BistroFox") == 0;
        const auto position = ecs.Get<TransformComponent>(entity).Location;
        require(position.x >= (fox ? 79.99f : -1090.01f) && position.x <= (fox ? 480.01f : -969.99f) &&
            position.z >= (fox ? -130.01f : -630.01f) && position.z <= (fox ? -29.99f : -269.99f),
            "Walking must remain on the verified street/interior route");
    });
    require(BistroCharacters::NPCCount.SetFromString("0") && BistroCharacters::FoxCount.SetFromString("0"),
        "Character disable rejected");
    scene.Update(world, 0);
    require(ecs.GetEntityCount() == 0, "Disabled characters must release all their entities");
    require(BistroCharacters::NPCCount.SetFromString("4") && BistroCharacters::FoxCount.SetFromString("1"),
        "Character defaults restore rejected");
}

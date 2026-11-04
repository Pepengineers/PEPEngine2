#pragma once

#include "../src/BistroCharacters.h"
#include <map>
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
    Matrix bodyInverseBind;
    bool bodyFound = false;
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
        require(transform.Location.y >= (fox ? 39.7f : 20) && transform.Location.y <= (fox ? 52 : 33),
            "Characters must stand on the authored Bistro floor/pavement");
        if (fox) firstFox = entity;
        else
        {
            if (firstNPC == InvalidEntity)
            {
                firstNPC = entity;
                sharedNPC = skeletal.GetScene();
                size_t parts = 0;
                for (const auto& node : source->GetNodes())
                    for (const auto handle : node.Meshes)
                    {
                        const auto* mesh = assets.Meshes().GetMesh(handle);
                        require(mesh != nullptr, "Soldier imported mesh missing");
                        for (const auto& part : mesh->GetSubMeshes())
                        {
                            for (const auto& bone : part.Bones)
                                if (bone.NodeName == "SoldierBone1")
                                { bodyInverseBind = bone.InverseBind; bodyFound = true; }
                            size_t outward = 0, inward = 0;
                            for (size_t index = 0; index < part.Indices.size(); index += 3)
                            {
                                const auto& a = part.Vertices.at(part.Indices.at(index));
                                const auto& b = part.Vertices.at(part.Indices.at(index + 1));
                                const auto& c = part.Vertices.at(part.Indices.at(index + 2));
                                const auto face = (b.Position - a.Position).Cross(c.Position - a.Position);
                                const float alignment = face.Dot(a.Normal + b.Normal + c.Normal);
                                outward += alignment > 1e-8f;
                                inward += alignment < -1e-8f;
                            }
                            require(outward > 0.9 * (outward + inward),
                                "Soldier front faces must agree with surface normals, not expose interior faces");
                            ++parts;
                        }
                    }
                require(parts == 5, "Soldier surface check must cover all five material sections");
            }
            require(skeletal.GetScene() == sharedNPC, "Human instances must share the imported scene asset");
        }
    });
    require(firstNPC != InvalidEntity && firstFox != InvalidEntity, "Both character populations are required");
    require(BistroCharacters::NPCCount.SetFromString("1"), "Single walker count rejected");
    scene.Update(world, 0);
    const auto npcStart = ecs.Get<TransformComponent>(firstNPC).Location;
    const auto foxStart = ecs.Get<TransformComponent>(firstFox).Location;
    scene.Update(world, 0.5f);
    require(ecs.Get<TransformComponent>(firstNPC).Location != npcStart &&
        ecs.Get<TransformComponent>(firstFox).Location != foxStart, "Both character populations must walk");
    const auto facingStart = ecs.Get<TransformComponent>(firstNPC).Location;
    scene.Update(world, 0.01f);
    const auto& npcTransform = ecs.Get<TransformComponent>(firstNPC);
    const auto& npcMesh = ecs.Get<SkeletalMeshRenderComponent>(firstNPC);
    const auto* npcSource = assets.Scenes().GetScene(npcMesh.GetScene());
    const auto animated = npcSource->EvaluatePose(npcMesh.GetClip(), 0.5, true);
    size_t hips = 0;
    while (hips < npcSource->GetNodes().size() && npcSource->GetNode(hips).Name != "SoldierBone1") ++hips;
    require(bodyFound && hips < animated.size(), "Soldier animated body root is missing");
    // Node defaults are Take1 frame zero; use the actual skin inverse bind, as the GPU does.
    const Matrix facing = bodyInverseBind * animated[hips] *
        Matrix::CreateRotationY(XMConvertToRadians(npcTransform.Rotation.y));
    auto forward = Vector3::TransformNormal(Vector3::UnitZ, facing);
    auto movement = npcTransform.Location - facingStart;
    forward.Normalize(); movement.Normalize();
    require(forward.Dot(movement) > 0.9f, "Animated soldier must walk forward along the street route");
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
    require(BistroCharacters::NPCCount.SetFromString("16") && BistroCharacters::FoxCount.SetFromString("8"),
        "Maximum character populations rejected");
    scene.Update(world, 0);
    require(ecs.GetPool<SkeletalMeshRenderComponent>().Size() == 24, "Maximum character count mismatch");
    std::map<Entity, std::pair<Vector3, float>> progress;
    for (int frame = 0; frame < 600; ++frame)
    {
        scene.Update(world, 0.1f);
        std::vector<std::pair<Vector3, bool>> positions;
        ecs.ForEach<SkeletalMeshRenderComponent, NameComponent>([&](Entity entity,
            const SkeletalMeshRenderComponent&, const NameComponent& name)
        {
            const bool fox = name.value.find("BistroFox") == 0;
            const auto& network = fox ? BistroRoutes::Foxes() : BistroRoutes::People();
            const auto position = ecs.Get<TransformComponent>(entity).Location;
            auto [entry, added] = progress.try_emplace(entity, position, 0.0f);
            entry->second.second += Vector3::Distance(entry->second.first, position);
            entry->second.first = position;
            bool onRoute = false;
            for (const auto& edge : network.Edges)
                for (size_t i = 1; i < edge.Points.size(); ++i)
                {
                    const auto travel = edge.Points[i] - edge.Points[i - 1];
                    const auto delta = position - edge.Points[i - 1];
                    const float t = std::clamp(delta.Dot(travel) / travel.LengthSquared(), 0.0f, 1.0f);
                    onRoute |= (delta - t * travel).LengthSquared() < 0.01f;
                }
            require(onRoute, "Walker left its spline network");
            for (const auto& [other, otherFox] : positions)
                require(fox != otherFox || BistroRoutes::HorizontalDistanceSquared(position, other) >=
                    network.Separation * network.Separation - 0.1f, "Walkers overlapped while spawning or moving");
            positions.emplace_back(position, fox);
        });
        // Exercise adding new walkers while the existing population is away from its spawn slots.
        if (frame == 300) { BistroCharacters::NPCCount.SetFromString("4"); BistroCharacters::FoxCount.SetFromString("1"); }
        if (frame == 301) { BistroCharacters::NPCCount.SetFromString("16"); BistroCharacters::FoxCount.SetFromString("8"); }
    }
    ecs.ForEach<SkeletalMeshRenderComponent>([&](Entity entity, const SkeletalMeshRenderComponent&)
    {
        require(progress.at(entity).second > 100, "Crowd traffic left a walker permanently stuck");
    });
    require(BistroCharacters::NPCCount.SetFromString("0") && BistroCharacters::FoxCount.SetFromString("0"),
        "Character disable rejected");
    scene.Update(world, 0);
    require(ecs.GetEntityCount() == 0, "Disabled characters must release all their entities");
    require(BistroCharacters::NPCCount.SetFromString("4") && BistroCharacters::FoxCount.SetFromString("1"),
        "Character defaults restore rejected");
}

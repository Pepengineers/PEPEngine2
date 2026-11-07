#pragma once

#include "../src/BistroCharacters.h"
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <tuple>
#include <iostream>
#include <atomic>
#include <chrono>
#include <stdexcept>

inline void CheckBistroParallelMovement()
{
    class ObservedScene : public BistroCharacters::Scene
    {
    public:
        using Scene::Scene;
        std::set<std::thread::id> Workers;
    protected:
        void Execute(size_t index) override
        {
            { std::lock_guard<std::mutex> guard(_lock); Workers.insert(std::this_thread::get_id()); }
            // Give workers time to pick up the first tiny batch instead of racing the caller.
            if (_warmup.fetch_add(1, std::memory_order_relaxed) < 24)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            Scene::Execute(index);
        }
    private:
        std::mutex _lock;
        std::atomic<unsigned> _warmup{0};
    };
    const auto snapshot = [](World& world)
    {
        std::map<std::string, std::tuple<Vector3, Vector3, double>> result;
        world.GetECS().ForEach<NameComponent, TransformComponent, SkeletalMeshRenderComponent>(
            [&](Entity, const NameComponent& name, const TransformComponent& transform, const SkeletalMeshRenderComponent& mesh)
            { result.emplace(name.value, std::make_tuple(transform.Location, transform.Rotation, mesh.GetPlaybackSpeed())); });
        return result;
    };
    JobSystem serialJobs(1), parallelJobs(4);
    World serialWorld(WorldDesc{}), parallelWorld(WorldDesc{});
    BistroCharacters::Scene serial(0x5eed);
    ObservedScene parallel(0x5eed);
    BistroCharacters::NPCCount.SetFromString(std::to_string(BistroRoutes::People().Spawns.size() * 2 + 1));
    BistroCharacters::FoxCount.SetFromString(std::to_string(BistroRoutes::Foxes().Spawns.size() + 5));
    for (int frame = 0; frame < 600; ++frame)
    {
        if (frame == 200) { BistroCharacters::NPCCount.SetFromString("0"); BistroCharacters::FoxCount.SetFromString("0"); }
        serial.Update(serialWorld, 0.1f, serialJobs);
        size_t sourceCount = 0;
        serialWorld.GetECS().ForEach<BistroInteractionSource, TransformComponent, NameComponent>(
            [&](Entity, const BistroInteractionSource& source, const TransformComponent& transform, const NameComponent& name)
            {
                if (name.value.find("BistroNPC") != 0 || Vector3(source.PositionRadius.x, source.PositionRadius.y,
                    source.PositionRadius.z) != transform.Location)
                    throw std::runtime_error("Interaction snapshot was published before movement or included a fox");
                ++sourceCount;
            });
        if (sourceCount != BistroRoutes::People().Spawns.size() * 2 + 1)
            throw std::runtime_error("Interaction snapshot silently dropped crowded rank members");
        parallel.Update(parallelWorld, 0.1f, parallelJobs);
        if (snapshot(serialWorld) != snapshot(parallelWorld))
            throw std::runtime_error("Crowd movement depends on worker scheduling/order");
    }
    if (parallel.Workers.size() < 2) throw std::runtime_error("Crowd jobs never used multiple threads");
    std::cout << "Bistro crowd worker threads: " << parallel.Workers.size() << "; 1/4-thread trajectories identical." << std::endl;
    BistroCharacters::NPCCount.SetFromString("10");
    BistroCharacters::FoxCount.SetFromString("5");
}

inline void CheckBistroCharacterCounts()
{
    const auto require = [](bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    };
    const auto& loop = BistroRoutes::People();
    float lap = 0;
    for (size_t i = 0; i < loop.Edges.size(); ++i)
    {
        require(loop.Incident[i].size() == 2 && loop.Edges[i].From == i &&
            loop.Edges[i].To == (i + 1) % loop.Nodes.size(), "Human route must be one ordered closed circuit");
        lap += loop.Edges[i].Length();
    }
    require(!loop.Spawns.empty(), "Short route segments must still create spawn positions");
    for (const auto& spawn : loop.Spawns)
    {
        auto cursor = spawn;
        loop.Advance(cursor, lap * 2);
        require(cursor.Forward && Vector3::Distance(loop.Position(cursor), loop.Position(spawn)) < 1,
            "Human walker must return to its start after two laps without reversing");
    }
    World world(WorldDesc{});
    JobSystem jobs(4);
    BistroCharacters::Scene scene;
    auto& ecs = world.GetECS();
    require(BistroCharacters::NPCCount.GetValue() == 10 && BistroCharacters::FoxCount.GetValue() == 5,
        "Bistro must default to ten humans and five foxes");
    require(!BistroCharacters::NPCCount.SetFromString("-1") && !BistroCharacters::NPCCount.SetFromString("1.5") && !BistroCharacters::FoxCount.SetFromString("-1"),
        "Invalid Bistro character count accepted");
    scene.Update(world, 0, jobs);
    require(ecs.GetPool<SkeletalMeshRenderComponent>().Size() == 15, "Default Bistro character count mismatch");
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
        require(transform.Location.y >= (fox ? 39.7f : 20) && transform.Location.y <= 50,
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
    scene.Update(world, 0, jobs);
    require(ecs.GetPool<SkeletalMeshRenderComponent>().Size() == 15, "Startup counts changed in an existing world");
    const auto npcStart = ecs.Get<TransformComponent>(firstNPC).Location;
    const auto foxStart = ecs.Get<TransformComponent>(firstFox).Location;
    scene.Update(world, 0.5f, jobs);
    require(ecs.Get<TransformComponent>(firstNPC).Location != npcStart &&
        ecs.Get<TransformComponent>(firstFox).Location != foxStart, "Both character populations must walk");
    const auto facingStart = ecs.Get<TransformComponent>(firstNPC).Location;
    scene.Update(world, 0.01f, jobs);
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
    scene.Update(world, 1, jobs);
    require(ecs.Get<TransformComponent>(firstNPC).Location == pausedNPC &&
        ecs.Get<TransformComponent>(firstFox).Location == pausedFox, "World pause must stop character movement");
    world.SetPaused(false);
    world.SetTimeScale(0);
    scene.Update(world, 1, jobs);
    require(ecs.Get<TransformComponent>(firstNPC).Location == pausedNPC, "Zero time scale must stop movement");
    world.SetTimeScale(1);
    require(BistroCharacters::NPCCount.SetFromString("0") && BistroCharacters::FoxCount.SetFromString("0"),
        "Character disable rejected");
    scene.Update(world, 0, jobs);
    require(ecs.GetPool<SkeletalMeshRenderComponent>().Size() == 15, "Startup count edit removed existing characters");
    World emptyWorld(WorldDesc{});
    BistroCharacters::Scene empty;
    empty.Update(emptyWorld, 0, jobs);
    require(emptyWorld.GetECS().GetEntityCount() == 0, "Zero startup counts must create no characters");
    require(BistroCharacters::NPCCount.SetFromString(std::to_string(BistroRoutes::People().Spawns.size() * 2 + 1)) &&
        BistroCharacters::FoxCount.SetFromString(std::to_string(BistroRoutes::Foxes().Spawns.size() + 5)),
        "Counts beyond the spawn list rejected");
    World crowdedWorld(WorldDesc{});
    BistroCharacters::Scene crowded(0x5eed);
    crowded.Update(crowdedWorld, 0, jobs);
    auto& crowd = crowdedWorld.GetECS();
    require(crowd.GetPool<SkeletalMeshRenderComponent>().Size() ==
        BistroRoutes::People().Spawns.size() * 2 + BistroRoutes::Foxes().Spawns.size() + 6,
        "Crowded startup must create the complete requested population");
    std::array<Entity, 3> rank = {InvalidEntity, InvalidEntity, InvalidEntity};
    std::vector<Vector3> foxSpawns;
    bool repeatedFoxSpawn = false;
    crowd.ForEach<NameComponent, TransformComponent>([&](Entity entity, const NameComponent& name, const TransformComponent& transform)
    {
        for (size_t member = 0; member < rank.size(); ++member)
            if (name.value == "BistroNPC" + std::to_string(member)) rank[member] = entity;
        if (name.value.find("BistroFox") != 0) return;
        for (const auto position : foxSpawns) repeatedFoxSpawn |= position == transform.Location;
        foxSpawns.push_back(transform.Location);
    });
    require(repeatedFoxSpawn, "Excess single-file foxes must reuse spawn positions");
    require(rank[0] != InvalidEntity && rank[1] != InvalidEntity && rank[2] != InvalidEntity, "Three-member human column missing");
    require(crowd.Get<SkeletalMeshRenderComponent>(rank[0]).GetTimeSeconds() !=
        crowd.Get<SkeletalMeshRenderComponent>(rank[1]).GetTimeSeconds(), "Rank members must retain individual animation phases");
    const auto onRoute = [](const BistroRoutes::Network& network, Vector3 position)
    {
        for (const auto& edge : network.Edges)
            for (size_t i = 1; i < edge.Points.size(); ++i)
            {
                const auto travel = edge.Points[i] - edge.Points[i - 1];
                const auto delta = position - edge.Points[i - 1];
                const float t = std::clamp(delta.Dot(travel) / travel.LengthSquared(), 0.0f, 1.0f);
                if ((delta - t * travel).LengthSquared() < 0.01f) return true;
            }
        return false;
    };
    std::map<Entity, std::pair<Vector3, float>> progress;
    for (int frame = 0; frame < 600; ++frame)
    {
        crowded.Update(crowdedWorld, 0.1f, jobs);
        const auto& left = crowd.Get<TransformComponent>(rank[0]);
        const auto& middle = crowd.Get<TransformComponent>(rank[1]);
        const auto& right = crowd.Get<TransformComponent>(rank[2]);
        require(onRoute(BistroRoutes::People(), left.Location) && onRoute(BistroRoutes::People(), middle.Location) &&
            onRoute(BistroRoutes::People(), right.Location), "Every column member must stay on the validated human route");
        require(Vector3::Distance(left.Location, middle.Location) > 20 &&
            Vector3::Distance(middle.Location, right.Location) > 20 &&
            Vector3::Distance(left.Location, middle.Location) <= 110.1f &&
            Vector3::Distance(middle.Location, right.Location) <= 110.1f,
            "Human column members must follow within their 110 cm arc spacing");
        crowd.ForEach<SkeletalMeshRenderComponent, NameComponent>([&](Entity entity,
            const SkeletalMeshRenderComponent& mesh, const NameComponent& name)
        {
            const auto position = crowd.Get<TransformComponent>(entity).Location;
            auto [entry, added] = progress.try_emplace(entity, position, 0.0f);
            entry->second.second += Vector3::Distance(entry->second.first, position);
            entry->second.first = position;
            require(mesh.GetPlaybackSpeed() == 1, "Crowded walkers must not stop each other's animation");
            if (name.value.find("BistroFox") == 0)
                require(onRoute(BistroRoutes::Foxes(), position), "Single-file fox left its route");
        });
    }
    for (const auto& [entity, distance] : progress)
        require(distance.second > 100, "Crowded benchmark left a walker stuck");
    CheckBistroParallelMovement();
}

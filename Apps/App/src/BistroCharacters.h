#pragma once

#include "App.Base/ECS/World.h"
#include "BistroRoutes.h"
#include "App.Base/Modules/PhysicsModule.h"
#include "Components/BistroInteractionSource.h"
#include "Engine.Core/AssetManager.h"
#include "Engine.Core/Jobs/JobSystem.h"
#include "Common/ConsoleVariables.h"
#include <cmath>
#include <vector>
#include <numeric>

namespace BistroCharacters
{
    inline AutoConsoleVariable<int> NPCCount(L"App.NPC.Count", 10,
        L"Initial people count; crowded groups follow in single file, 0 disables, applies on startup",
        EConsoleVariableFlags::ReadOnly, [](int value) { return value >= 0; });
    inline AutoConsoleVariable<int> FoxCount(L"App.Fox.Count", 5,
        L"Initial single-file fox count; spawn positions repeat when crowded, 0 disables, applies on startup",
        EConsoleVariableFlags::ReadOnly, [](int value) { return value >= 0; });

    class Scene : private ParallelJob
    {
        struct Group
        {
            BistroRoutes::Cursor Route;
            size_t First, Count;
            bool Fox;
        };

    public:
        explicit Scene(unsigned seed = std::random_device{}()) : _random(seed) {}

        void Update(World& world, float deltaSeconds, JobSystem& jobs, PhysicsModule* physics = nullptr)
        {
            const bool spawning = !_initialized;
            auto& ecs = world.GetECS();
            ecs.RegisterComponent<BistroInteractionSource>();
            if (spawning)
            {
                Spawn(ecs, static_cast<size_t>(NPCCount.GetValue()), false);
                Spawn(ecs, static_cast<size_t>(FoxCount.GetValue()), true);
                _initialized = true;
            }
            _stepSeconds = world.IsPaused() ? 0 : deltaSeconds * world.GetTimeScale();
            if (_groups.empty() || (!spawning && _stepSeconds <= 0)) return;
            _world = &world;
            _physics = physics;
            _transforms.clear();
            for (const auto entity : _entities) _transforms.push_back(&ecs.Get<TransformComponent>(entity));
            Schedule(jobs, _groups.size(), 1).Complete();
            // Publish the complete snapshot only after every movement job has finished.
            for (const auto& group : _groups)
                if (!group.Fox)
                    for (size_t member = 0; member < group.Count; ++member)
                    {
                        auto& source = ecs.Get<BistroInteractionSource>(_entities[group.First + member]);
                        const auto position = _transforms[group.First + member]->Location;
                        const Vector3 previous(source.PositionRadius.x, source.PositionRadius.y, source.PositionRadius.z);
                        const auto start = spawning || Vector3::Distance(position, previous) > 500 ? position : previous;
                        source.PreviousHeight = XMFLOAT4(start.x, start.y, start.z, 190);
                        source.PositionRadius = XMFLOAT4(position.x, position.y, position.z, 65);
                    }
        }

    protected:
        void Execute(size_t index) override
        {
            auto& group = _groups[index];
            const auto& network = group.Fox ? BistroRoutes::Foxes() : BistroRoutes::People();
            network.Advance(group.Route, (group.Fox ? 60 : 95) * _stepSeconds);
            auto route = group.Route;
            for (size_t member = 0; member < group.Count; ++member)
            {
                if (member) network.Advance(route, -network.Separation);
                auto position = network.Position(route);
                auto sample = route;
                network.Advance(sample, 1);
                auto forward = network.Position(sample) - position;
                forward.y = 0;
                const float yaw = XMConvertToDegrees(std::atan2(forward.x, forward.z)) + (group.Fox ? 0 : 180);
                if (!group.Fox && _physics)
                {
                    PhysicsRayHit hit;
                    // Query near the authored paving, below ignored street furniture and fence rails.
                    if (!_physics->RayCastStatic(*_world, position + Vector3(0, 5, 0), {0, -55, 0}, hit) ||
                        hit.Normal.y <= 0 || std::abs(hit.Position.y + BistroRoutes::PeopleGroundOffset - position.y) >= 20)
                        throw std::runtime_error("Bistro human route lost its walkable ground");
                    position.y = hit.Position.y + BistroRoutes::PeopleGroundOffset;
                }
                auto& transform = *_transforms[group.First + member];
                transform.Location = position;
                transform.Rotation = Vector3(0, yaw, 0);
                transform.DirtyFlag = true;
            }
        }

    private:
        void Spawn(WorldECS& ecs, size_t requested, bool fox)
        {
            if (!requested) return;
            auto& assets = Engine::Core::AssetManager::GetInstance();
            const auto path = std::filesystem::path(ASSETS_FOLDER) /
                (fox ? "Models/KhronosFox/glTF/Fox.gltf" : "Models/MGPUSoldier/Soldier.gltf");
            const auto* source = assets.LoadScene(path);
            if (!source) throw std::runtime_error("Cannot load Bistro character: " + path.string());
            const auto handle = assets.Scenes().FindHandle(path);
            int clip = -1;
            for (size_t index = 0; index < source->GetClips().size(); ++index)
                if (source->GetClips()[index].Name == (fox ? "Walk" : "Take1")) clip = static_cast<int>(index);
            if (!handle.IsValid() || clip < 0) throw std::runtime_error("Bistro character walking clip is missing");
            const auto& network = fox ? BistroRoutes::Foxes() : BistroRoutes::People();
            if (network.Spawns.empty()) throw std::runtime_error("Bistro character route has no spawn positions");
            std::vector<size_t> slots(network.Spawns.size());
            std::iota(slots.begin(), slots.end(), size_t{0});
            std::shuffle(slots.begin(), slots.end(), _random);
            const size_t groups = fox ? requested : (std::min)(requested, slots.size());
            size_t character = 0;
            for (size_t index = 0; index < groups; ++index)
            {
                auto route = network.Spawns[slots[index % slots.size()]];
                route.Forward = true;
                const size_t members = requested / groups + (index < requested % groups);
                _groups.push_back({route, _entities.size(), members, fox});
                for (size_t member = 0; member < members; ++member, ++character)
                {
                    auto entity = ecs.CreateEntity();
                    entity.AddComponent<NameComponent>((fox ? "BistroFox" : "BistroNPC") + std::to_string(character));
                    entity.AddComponent<TransformComponent>(network.Position(route), Vector3::Zero, Vector3(fox ? 0.55f : 2.4f));
                    auto& skeletal = entity.AddComponent<SkeletalMeshRenderComponent>(handle);
                    skeletal.SetClip(clip);
                    skeletal.SetLooping(true);
                    skeletal.SetTimeSeconds(std::uniform_real_distribution<double>(0, source->GetClips()[clip].DurationSeconds)(_random));
                    if (!fox) entity.AddComponent<BistroInteractionSource>();
                    _entities.push_back(entity.GetId());
                }
            }
        }

        std::mt19937 _random;
        std::vector<Group> _groups;
        std::vector<Entity> _entities;
        std::vector<TransformComponent*> _transforms;
        World* _world = nullptr;
        PhysicsModule* _physics = nullptr;
        float _stepSeconds = 0;
        bool _initialized = false;
    };
}

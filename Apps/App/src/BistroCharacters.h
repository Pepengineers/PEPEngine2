#pragma once

#include "App.Base/ECS/World.h"
#include "BistroRoutes.h"
#include "Engine.Core/AssetManager.h"
#include "Engine.Core/Jobs/JobSystem.h"
#include "Common/ConsoleVariables.h"
#include <cmath>
#include <vector>
#include <numeric>

namespace BistroCharacters
{
    inline AutoConsoleVariable<int> NPCCount(L"App.NPC.Count", 10,
        L"Initial people count; crowded spawns form wider ranks, 0 disables, applies on startup",
        EConsoleVariableFlags::ReadOnly, [](int value) { return value >= 0; });
    inline AutoConsoleVariable<int> FoxCount(L"App.Fox.Count", 5,
        L"Initial single-file fox count; spawn positions repeat when crowded, 0 disables, applies on startup",
        EConsoleVariableFlags::ReadOnly, [](int value) { return value >= 0; });

    class Scene : private ParallelJob
    {
        struct Group
        {
            BistroRoutes::Cursor Route;
            std::mt19937 Random;
            size_t First, Count;
            bool Fox;
        };

    public:
        explicit Scene(unsigned seed = std::random_device{}()) : _random(seed) {}

        void Update(World& world, float deltaSeconds, JobSystem& jobs)
        {
            const bool spawning = !_initialized;
            auto& ecs = world.GetECS();
            if (spawning)
            {
                Spawn(ecs, static_cast<size_t>(NPCCount.GetValue()), false);
                Spawn(ecs, static_cast<size_t>(FoxCount.GetValue()), true);
                _initialized = true;
            }
            _stepSeconds = world.IsPaused() ? 0 : deltaSeconds * world.GetTimeScale();
            if (_groups.empty() || (!spawning && _stepSeconds <= 0)) return;
            _transforms.clear();
            for (const auto entity : _entities) _transforms.push_back(&ecs.Get<TransformComponent>(entity));
            Schedule(jobs, _groups.size(), 1).Complete();
        }

    protected:
        void Execute(size_t index) override
        {
            auto& group = _groups[index];
            const auto& network = group.Fox ? BistroRoutes::Foxes() : BistroRoutes::People();
            network.Advance(group.Route, (group.Fox ? 60 : 95) * _stepSeconds, group.Random);
            const auto center = network.Position(group.Route);
            auto sample = group.Route;
            const float length = network.Edges[sample.Edge].Length();
            sample.Distance = sample.Distance < length ? (std::min)(length, sample.Distance + 1) :
                (std::max)(0.0f, sample.Distance - 1);
            auto forward = network.Position(sample) - center;
            if ((sample.Distance < group.Route.Distance) == group.Route.Forward) forward = -forward;
            forward.y = 0;
            forward.Normalize();
            const Vector3 right(forward.z, 0, -forward.x);
            const float yaw = XMConvertToDegrees(std::atan2(forward.x, forward.z)) + (group.Fox ? 0 : 180);
            for (size_t member = 0; member < group.Count; ++member)
            {
                auto& transform = *_transforms[group.First + member];
                const float offset = (static_cast<float>(member) - static_cast<float>(group.Count - 1) * 0.5f) * network.Separation;
                transform.Location = center + right * offset;
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
                route.Forward = std::uniform_int_distribution<int>(0, 1)(_random) != 0;
                const size_t members = requested / groups + (index < requested % groups);
                _groups.push_back({route, std::mt19937{_random()}, _entities.size(), members, fox});
                for (size_t member = 0; member < members; ++member, ++character)
                {
                    auto entity = ecs.CreateEntity();
                    entity.AddComponent<NameComponent>((fox ? "BistroFox" : "BistroNPC") + std::to_string(character));
                    entity.AddComponent<TransformComponent>(network.Position(route), Vector3::Zero, Vector3(fox ? 0.55f : 2.4f));
                    auto& skeletal = entity.AddComponent<SkeletalMeshRenderComponent>(handle);
                    skeletal.SetClip(clip);
                    skeletal.SetLooping(true);
                    skeletal.SetTimeSeconds(std::uniform_real_distribution<double>(0, source->GetClips()[clip].DurationSeconds)(_random));
                    _entities.push_back(entity.GetId());
                }
            }
        }

        std::mt19937 _random;
        std::vector<Group> _groups;
        std::vector<Entity> _entities;
        std::vector<TransformComponent*> _transforms;
        float _stepSeconds = 0;
        bool _initialized = false;
    };
}

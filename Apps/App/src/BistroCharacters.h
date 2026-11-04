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
    inline AutoConsoleVariable<int> NPCCount(L"App.NPC.Count", 4,
        L"People on random separated street/plaza spline positions, 0 disables, maximum 16",
        EConsoleVariableFlags::Default, [](int value) { return value >= 0 && value <= 16; });
    inline AutoConsoleVariable<int> FoxCount(L"App.Fox.Count", 1,
        L"Foxes on random separated interior spline positions, 0 disables, maximum 8",
        EConsoleVariableFlags::Default, [](int value) { return value >= 0 && value <= 8; });

    class Scene : private ParallelJob
    {
        struct Character { Entity Id; BistroRoutes::Cursor Route; std::mt19937 Random; };
        struct Step
        {
            Character* Actor;
            TransformComponent* Transform;
            SkeletalMeshRenderComponent* Mesh;
            const BistroRoutes::Network* Network;
            Vector3 Start, Position;
            BistroRoutes::Cursor Next;
            bool Fox;
        };

    public:
        explicit Scene(unsigned seed = std::random_device{}()) : _random(seed) {}

        void Update(World& world, float deltaSeconds, JobSystem& jobs)
        {
            auto& ecs = world.GetECS();
            Reconcile(ecs, _npcs, static_cast<size_t>(NPCCount.GetValue()), false);
            Reconcile(ecs, _foxes, static_cast<size_t>(FoxCount.GetValue()), true);
            if (_npcs.empty() && _foxes.empty()) return;
            float remaining = world.IsPaused() ? 0 : deltaSeconds * world.GetTimeScale();
            while (remaining > 0)
            {
                const float step = (std::min)(remaining, 0.1f);
                _stepSeconds = step;
                _steps.clear();
                Snapshot(ecs, _npcs, BistroRoutes::People(), false);
                Snapshot(ecs, _foxes, BistroRoutes::Foxes(), true);
                _apply = false;
                Schedule(jobs, _steps.size(), 1).Complete();
                _apply = true;
                Schedule(jobs, _steps.size(), 1).Complete();
                remaining -= step;
            }
        }

    private:
        static void Face(TransformComponent& transform, Vector3 movement, bool fox)
        {
            if (movement.x * movement.x + movement.z * movement.z < 0.000001f) return;
            transform.Rotation = Vector3(0, XMConvertToDegrees(std::atan2(movement.x, movement.z)) + (fox ? 0 : 180), 0);
        }

        void Snapshot(WorldECS& ecs, std::vector<Character>& characters, const BistroRoutes::Network& network, bool fox)
        {
            for (auto& actor : characters)
            {
                auto& transform = ecs.Get<TransformComponent>(actor.Id);
                _steps.push_back({&actor, &transform, &ecs.Get<SkeletalMeshRenderComponent>(actor.Id),
                    &network, transform.Location, {}, actor.Route, fox});
            }
        }

        static float ClosestDistanceSquared(Vector3 delta, Vector3 travel)
        {
            delta.y = travel.y = 0;
            const float t = travel.LengthSquared() > 0 ? std::clamp(-delta.Dot(travel) / travel.LengthSquared(), 0.0f, 1.0f) : 0;
            return (delta + travel * t).LengthSquared();
        }

    protected:
        void Execute(size_t index) override
        {
            auto& step = _steps[index];
            auto& actor = *step.Actor;
            if (!_apply)
            {
                step.Network->Advance(step.Next, (step.Fox ? 60 : 95) * _stepSeconds, actor.Random);
                step.Position = step.Network->Position(step.Next);
                return;
            }
            const auto travel = step.Position - step.Start;
            const float separationSquared = step.Network->Separation * step.Network->Separation;
            bool free = true;
            for (size_t other = 0; other < _steps.size(); ++other)
            {
                const auto& peer = _steps[other];
                if (other == index || peer.Fox != step.Fox) continue;
                const auto delta = step.Start - peer.Start;
                // Check both moving and stationary peers: a rejected proposal must also remain safe.
                free &= ClosestDistanceSquared(delta, travel) >= separationSquared &&
                    ClosestDistanceSquared(delta, travel - (peer.Position - peer.Start)) >= separationSquared;
            }
            if (free)
            {
                Face(*step.Transform, travel, step.Fox);
                step.Transform->Location = step.Position;
                step.Transform->DirtyFlag = true;
                actor.Route = step.Next;
                actor.Route.Waiting = 0;
            }
            else
            {
                actor.Route.Waiting += _stepSeconds;
                if (actor.Route.Waiting >= 1)
                {
                    actor.Route.Forward = !actor.Route.Forward;
                    actor.Route.Waiting = 0;
                }
            }
            const double playback = free ? 1 : 0;
            if (step.Mesh->GetPlaybackSpeed() != playback) step.Mesh->SetPlaybackSpeed(playback);
        }

    private:
        void Reconcile(WorldECS& ecs, std::vector<Character>& entities, size_t requested, bool fox)
        {
            while (entities.size() > requested)
            {
                ecs.DestroyEntity(entities.back().Id);
                entities.pop_back();
            }
            if (entities.size() == requested) return;
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
            while (entities.size() < requested)
            {
                const size_t index = entities.size();
                std::vector<size_t> slots(network.Spawns.size());
                std::iota(slots.begin(), slots.end(), size_t{0});
                std::shuffle(slots.begin(), slots.end(), _random);
                auto chosen = slots.end();
                for (auto slot = slots.begin(); slot != slots.end(); ++slot)
                {
                    bool free = true;
                    for (const auto& other : entities)
                        free &= BistroRoutes::HorizontalDistanceSquared(network.Position(network.Spawns[*slot]),
                            ecs.Get<TransformComponent>(other.Id).Location) >= network.Separation * network.Separation;
                    if (free) { chosen = slot; break; }
                }
                if (chosen == slots.end()) throw std::runtime_error("No separated Bistro character spawn slot");
                auto route = network.Spawns[*chosen];
                route.Forward = std::uniform_int_distribution<int>(0, 1)(_random) != 0;
                const auto position = network.Position(route);
                auto entity = ecs.CreateEntity();
                entity.AddComponent<NameComponent>((fox ? "BistroFox" : "BistroNPC") + std::to_string(index));
                auto& transform = entity.AddComponent<TransformComponent>(position, Vector3::Zero, Vector3(fox ? 0.55f : 2.4f));
                auto facing = route;
                network.Advance(facing, 1, _random);
                Face(transform, network.Position(facing) - position, fox);
                auto& skeletal = entity.AddComponent<SkeletalMeshRenderComponent>(handle);
                skeletal.SetClip(clip);
                skeletal.SetLooping(true);
                skeletal.SetTimeSeconds(std::uniform_real_distribution<double>(0, source->GetClips()[clip].DurationSeconds)(_random));
                entities.push_back({entity.GetId(), route, std::mt19937{_random()}});
            }
        }

        std::mt19937 _random{std::random_device{}()};
        std::vector<Character> _npcs, _foxes;
        std::vector<Step> _steps;
        float _stepSeconds = 0;
        bool _apply = false;
    };
}

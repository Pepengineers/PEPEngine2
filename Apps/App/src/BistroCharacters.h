#pragma once

#include "App.Base/ECS/World.h"
#include "BistroRoutes.h"
#include "Engine.Core/AssetManager.h"
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

    class Scene
    {
        struct Character { Entity Id; BistroRoutes::Cursor Route; };

    public:
        void Update(World& world, float deltaSeconds)
        {
            auto& ecs = world.GetECS();
            Reconcile(ecs, _npcs, static_cast<size_t>(NPCCount.GetValue()), false);
            Reconcile(ecs, _foxes, static_cast<size_t>(FoxCount.GetValue()), true);
            float remaining = world.IsPaused() ? 0 : deltaSeconds * world.GetTimeScale();
            while (remaining > 0)
            {
                const float step = (std::min)(remaining, 0.1f);
                Move(ecs, _npcs, BistroRoutes::People(), step, false);
                Move(ecs, _foxes, BistroRoutes::Foxes(), step, true);
                remaining -= step;
            }
        }

    private:
        static void Face(TransformComponent& transform, Vector3 movement, bool fox)
        {
            if (movement.x * movement.x + movement.z * movement.z < 0.000001f) return;
            transform.Rotation = Vector3(0, XMConvertToDegrees(std::atan2(movement.x, movement.z)) + (fox ? 0 : 180), 0);
        }

        void Move(WorldECS& ecs, std::vector<Character>& characters, const BistroRoutes::Network& network, float dt, bool fox)
        {
            for (auto& character : characters)
            {
                auto& transform = ecs.Get<TransformComponent>(character.Id);
                auto next = character.Route;
                network.Advance(next, (fox ? 60 : 95) * dt, _random);
                const auto position = network.Position(next);
                auto travel = position - transform.Location;
                travel.y = 0;
                bool free = true;
                for (const auto& other : characters)
                {
                    if (other.Id == character.Id) continue;
                    auto delta = ecs.Get<TransformComponent>(other.Id).Location - transform.Location;
                    delta.y = 0;
                    const float fraction = travel.LengthSquared() > 0 ? std::clamp(delta.Dot(travel) / travel.LengthSquared(), 0.0f, 1.0f) : 0;
                    free &= (delta - travel * fraction).LengthSquared() >= network.Separation * network.Separation;
                }
                if (free)
                {
                    Face(transform, position - transform.Location, fox);
                    transform.Location = position;
                    transform.DirtyFlag = true;
                    character.Route = next;
                    character.Route.Waiting = 0;
                }
                else
                {
                    character.Route.Waiting += dt;
                    if (character.Route.Waiting >= 1)
                    {
                        character.Route.Forward = !character.Route.Forward;
                        character.Route.Waiting = 0;
                    }
                }
                auto& skeletal = ecs.Get<SkeletalMeshRenderComponent>(character.Id);
                const double playback = free ? 1 : 0;
                if (skeletal.GetPlaybackSpeed() != playback) skeletal.SetPlaybackSpeed(playback);
            }
        }

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
                entities.push_back({entity.GetId(), route});
            }
        }

        std::mt19937 _random{std::random_device{}()};
        std::vector<Character> _npcs, _foxes;
    };
}

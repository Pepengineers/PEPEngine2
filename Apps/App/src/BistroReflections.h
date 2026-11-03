#pragma once

#include "App.Base/ECS/World.h"
#include "Common/ConsoleVariables.h"
#include <array>
#include <vector>

namespace BistroReflections
{
    inline AutoConsoleVariable<int> Count(L"App.ReflectionProbes.Count", 4,
        L"Frozen static Bistro cubemaps, 0 disables, maximum 32",
        EConsoleVariableFlags::Default, [](int value) { return value >= 0 && value <= static_cast<int>(MaxReflectionProbes); });

    class Scene
    {
    public:
        void Update(World& world, float)
        {
            auto& ecs = world.GetECS();
            const size_t requested = static_cast<size_t>(Count.GetValue());
            while (_entities.size() > requested)
            {
                ecs.DestroyEntity(_entities.back());
                _entities.pop_back();
            }
            while (_entities.size() < requested)
            {
                const size_t index = _entities.size();
                const std::array<Vector3, 4> positions = {
                    Vector3(-280, 160, -170), Vector3(-600, 180, -500),
                    Vector3(250, 170, -100), Vector3(650, 170, -350) };
                const Vector3 offset(-450.0f * static_cast<float>(index / positions.size()), 0,
                    450.0f * static_cast<float>(index / positions.size()));
                auto entity = ecs.CreateEntity();
                entity.AddComponent<NameComponent>("BistroReflection" + std::to_string(index));
                entity.AddComponent<TransformComponent>(positions[index % positions.size()] + offset);
                auto& probe = entity.AddComponent<ReflectionProbeComponent>();
                probe.Implementation = 2;
                probe.Extents = index % positions.size() < 2 ? Vector3(650, 300, 600) : Vector3(400, 250, 400);
                probe.BlendDistance = 150;
                probe.Resolution = 128;
                probe.FacesPerFrame = 6;
                probe.PrefilterSamples = 64;
                probe.FarPlane = 15000;
                _entities.push_back(entity.GetId());
            }
        }

    private:
        std::vector<Entity> _entities;
    };
}

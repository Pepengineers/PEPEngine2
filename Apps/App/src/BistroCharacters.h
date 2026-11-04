#pragma once

#include "App.Base/ECS/World.h"
#include "Engine.Core/AssetManager.h"
#include "Common/ConsoleVariables.h"
#include <cmath>
#include <vector>

namespace BistroCharacters
{
    inline AutoConsoleVariable<int> NPCCount(L"App.NPC.Count", 4,
        L"Walking MGPU soldiers outside Bistro, 0 disables, maximum 16",
        EConsoleVariableFlags::Default, [](int value) { return value >= 0 && value <= 16; });
    inline AutoConsoleVariable<int> FoxCount(L"App.Fox.Count", 1,
        L"Walking foxes inside Bistro, 0 disables, maximum 8",
        EConsoleVariableFlags::Default, [](int value) { return value >= 0 && value <= 8; });

    // Constant-speed capsule: two straight sections joined by semicircles.
    inline void PlaceOnRoute(TransformComponent& transform, double distance, float halfStraight,
        float radius, Vector3 center, bool alongX)
    {
        const double straight = 2.0 * halfStraight;
        const double arc = XM_PI * radius;
        const double perimeter = 2.0 * (straight + arc);
        double cursor = std::fmod(distance, perimeter);
        if (cursor < 0) cursor += perimeter;
        float x, z, dx, dz;
        if (cursor < straight)
        {
            x = -radius; z = -halfStraight + static_cast<float>(cursor); dx = 0; dz = 1;
        }
        else if ((cursor -= straight) < arc)
        {
            const float angle = static_cast<float>(cursor / radius);
            x = -radius * std::cos(angle); z = halfStraight + radius * std::sin(angle);
            dx = std::sin(angle); dz = std::cos(angle);
        }
        else if ((cursor -= arc) < straight)
        {
            x = radius; z = halfStraight - static_cast<float>(cursor); dx = 0; dz = -1;
        }
        else
        {
            const float angle = static_cast<float>((cursor - straight) / radius);
            x = radius * std::cos(angle); z = -halfStraight - radius * std::sin(angle);
            dx = -std::sin(angle); dz = -std::cos(angle);
        }
        if (alongX) { std::swap(x, z); std::swap(dx, dz); }
        transform.Location = center + Vector3(x, 0, z);
        transform.Rotation = Vector3(0, XMConvertToDegrees(std::atan2(dx, dz)), 0);
        transform.DirtyFlag = true;
    }

    class Scene
    {
    public:
        void Update(World& world, float deltaSeconds)
        {
            auto& ecs = world.GetECS();
            Reconcile(ecs, _npcs, static_cast<size_t>(NPCCount.GetValue()), false);
            Reconcile(ecs, _foxes, static_cast<size_t>(FoxCount.GetValue()), true);
            if (!world.IsPaused()) _time += static_cast<double>(deltaSeconds) * world.GetTimeScale();
            for (size_t index = 0; index < _npcs.size(); ++index)
            {
                const float halfStraight = 120, radius = 60;
                const double perimeter = 4 * halfStraight + 2 * XM_PI * radius;
                // This capsule stays on the OBJ's flat pavement, above the model's source-foot offset.
                const Vector3 center(-1030, 23.162f + 4.6f, -450);
                const double phase = (index % 4) / 4.0 + (index / 4) / 16.0;
                auto& transform = ecs.Get<TransformComponent>(_npcs[index]);
                PlaceOnRoute(transform, _time * 95 + perimeter * phase,
                    halfStraight, radius, center, false);
                // Take1 turns the body toward local -Z relative to its original skin bind pose.
                transform.Rotation.y += 180;
            }
            for (size_t index = 0; index < _foxes.size(); ++index)
            {
                const float halfStraight = 150, radius = 50;
                const double perimeter = 4 * halfStraight + 2 * XM_PI * radius;
                const Vector3 center(280, 39.622f + 0.1f, -80);
                const double phase = (index % 4) / 4.0 + (index / 4) / 8.0;
                PlaceOnRoute(ecs.Get<TransformComponent>(_foxes[index]), _time * 60 + perimeter * phase,
                    halfStraight, radius, center, true);
            }
        }

    private:
        void Reconcile(WorldECS& ecs, std::vector<Entity>& entities, size_t requested, bool fox)
        {
            while (entities.size() > requested)
            {
                ecs.DestroyEntity(entities.back());
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
            while (entities.size() < requested)
            {
                const size_t index = entities.size();
                auto entity = ecs.CreateEntity();
                entity.AddComponent<NameComponent>((fox ? "BistroFox" : "BistroNPC") + std::to_string(index));
                entity.AddComponent<TransformComponent>(Vector3::Zero, Vector3::Zero, Vector3(fox ? 0.55f : 2.4f));
                auto& skeletal = entity.AddComponent<SkeletalMeshRenderComponent>(handle);
                skeletal.SetClip(clip);
                skeletal.SetLooping(true);
                skeletal.SetTimeSeconds(static_cast<double>(index % 4) * 0.27);
                entities.push_back(entity.GetId());
            }
        }

        double _time = 0;
        std::vector<Entity> _npcs, _foxes;
    };
}

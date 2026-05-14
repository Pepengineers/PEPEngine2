#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <utility>
#include <vector>

#include "Common/GameTimer.h"
#include "Common/Module.h"
#include "Common/ConsoleVariables.h"

#include "App.Base/World.h"

class World;
class System;

class SceneManagerModule final : public Module
{
public:
    struct SystemEntry
    {
        uint8_t priority = 0;
        World* world = nullptr;
        std::unique_ptr<System> system;
    };

    SceneManagerModule(GameTimer* timer);
    ~SceneManagerModule() override;

    void Initialize() override;
    void Uninitialize() override;

    bool LoadWorld(const std::filesystem::path& path);
    bool UnloadWorld(size_t index);
    bool SaveWorld(World* world, const std::filesystem::path& path);
    World* GetWorld(size_t index = 0);

    template<typename T, typename... Args>
    void AddSystem(World* world, uint8_t priority, Args&&... args) {
        SystemEntry entry;
        entry.priority = priority;
        entry.world = world;
        entry.system = std::make_unique<T>(std::forward<Args>(args)...);

        _systemVector.push_back(std::move(entry));

        std::stable_sort(_systemVector.begin(), _systemVector.end(),
            [](const SystemEntry& a, const SystemEntry& b) {
                return a.priority < b.priority;
            });
    }

protected:
    void OnUpdate() override;
    void OnRender() override;

    bool ShouldTick() override;
    bool ShouldRender() override;

private:
    void Tick(float dt);

    GameTimer* _timer;
    std::vector<std::unique_ptr<World>> _worldVector;
    std::vector<SystemEntry> _systemVector;
};

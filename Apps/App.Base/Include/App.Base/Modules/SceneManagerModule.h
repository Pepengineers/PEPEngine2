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

#include "Engine.Core/ECS/Event.h"
#include "App.Base/ECS/World.h"
#include "App.Base/ECS/AppConfig.h"

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
    
    Event<World&> OnWorldCreated;
    Event<World&> OnWorldDestroyed;

    SceneManagerModule(GameTimer* timer);
    ~SceneManagerModule() override;

    void Initialize() override;
    void Uninitialize() override;

    bool LoadScene(const std::string& scenePath, const AppConfig& appConfig);

    bool LoadWorld(const std::filesystem::path& path);
    bool UnloadWorld(size_t index);
    bool SaveWorld(World* world, const std::filesystem::path& path);
    World* GetWorld(size_t index = 0);
    
    size_t GetWorldCount() const;

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
            }
        );
    }

protected:
    void OnUpdate() override;
    void OnRender() override;

    bool ShouldTick() override;
    bool ShouldRender() override;

private:
    void Tick(float dt);

    bool LoadConfiguredWorld(
        const SceneWorldConfig& sceneWorldConfig,
        const AppConfig& appConfig
    );

    bool AddSystemsFromConfig(
        World* world,
        const WorldConfig& worldConfig,
        const AppConfig& appConfig
    );

    bool AddSystemByName(
        World* world,
        const SystemConfig& systemConfig
    );

    static uint8_t ToSystemPriority(int priority);

private:
    GameTimer* _timer;
    std::vector<std::unique_ptr<World>> _worldVector;
    std::vector<SystemEntry> _systemVector;
};

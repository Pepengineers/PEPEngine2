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

class World;
class System;

class SceneManagerModule final : public Module
{
public:
    Event<World&> OnWorldCreated;
    Event<World&> OnWorldDestroyed;

    explicit SceneManagerModule(std::filesystem::path scenePath = {});
    ~SceneManagerModule() override;

    void Initialize() override;
    void Uninitialize() override;

    bool LoadWorld(const std::filesystem::path& path);
    bool UnloadWorld(size_t index);
    bool SaveWorld(World* world, const std::filesystem::path& path);
    World* GetWorld(size_t index = 0);
    
    size_t GetWorldCount() const;

protected:
    void OnUpdate() override;
    void OnRender() override;

    bool ShouldTick() override;
    bool ShouldRender() override;

private:

    std::filesystem::path _scenePath;
    std::vector<std::unique_ptr<World>> _worldVector;
};

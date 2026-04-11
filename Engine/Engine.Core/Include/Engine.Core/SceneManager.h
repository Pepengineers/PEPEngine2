#pragma once
#include <vector>
#include <memory>
#include <filesystem>
#include <cstdint>
#include <algorithm>

class World;
class System;

class SceneManager
{
public:
    bool LoadWorld(const std::filesystem::path& path);

    bool UnloadWorld(size_t index);

    bool SaveWorld(World* world, const std::filesystem::path& path);

    World* GetWorld(size_t index = 0);

    struct SystemEntry
    {
        uint8_t priority = 0;
        World* world = nullptr;
        std::unique_ptr<System> system;
    };

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

    void Tick(float dt);

private:
    std::vector<std::unique_ptr<World>> _worldVector;
    std::vector<SystemEntry> _systemVector;
};
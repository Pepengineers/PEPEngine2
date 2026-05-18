#pragma once
#include <string>
#include <filesystem>
#include <cstdint>

#include "App.Base/ECSStorage.h"
#include "App.Base/Components/TransformComponent.h"
#include "App.Base/Components/VelocityComponent.h"
#include "App.Base/Components/NameComponent.h"
#include "App.Base/Components/StaticMeshRenderComponent.h"
#include "App.Base/Components/CameraComponent.h"

struct WorldDesc {
    std::filesystem::path WorldFilePath;
    std::string Name;
    // todo add obj count and some basic info
};

using WorldECS = ECSStorage<
    TransformComponent,
    VelocityComponent,
    NameComponent,
    StaticMeshRenderComponent,
    CameraComponent
>;

class World {
public:
    World(const WorldDesc& desc)
        : _desc(desc)
    {
    }

    bool Load();

    bool Save(std::filesystem::path path);
    
    void Tick(float dt);

    void SetTimeScale(float scale);
    float GetTimeScale() const;

    void SetPaused(bool paused);
    bool IsPaused() const;

    WorldECS& GetECS();
    const WorldECS& GetECS() const;

    void SetName(std::string name);

    const std::filesystem::path& GetPath() const;

    Entity ActiveCamera;

private:
    
    WorldDesc _desc;

    WorldECS _ecs;

    float _timeScale = 1.0f;
    bool _bPaused = false;
};
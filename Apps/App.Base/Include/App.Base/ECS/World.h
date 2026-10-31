#pragma once
#include <string>
#include <filesystem>
#include <cstdint>
#include <atomic>

#include "Engine.Core/ECS/ECSStorage.h"
#include "App.Base/ECS/Components/CameraComponent.h"
#include "App.Base/ECS/Components/CircleMovementComponent.h"
#include "App.Base/ECS/Components/LookAtTargetComponent.h"
#include "App.Base/ECS/Components/LightComponent.h"
#include "App.Base/ECS/Components/ReflectionProbeComponent.h"
#include "App.Base/ECS/Components/FlyingPropComponent.h"
#include "App.Base/ECS/Components/NameComponent.h"
#include "App.Base/ECS/Components/SplineCurveComponent.h"
#include "App.Base/ECS/Components/SplineFollowComponent.h"
#include "App.Base/ECS/Components/StaticMeshRenderComponent.h"
#include "App.Base/ECS/Components/TransformComponent.h"
#include "App.Base/ECS/Components/VelocityComponent.h"

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
    CameraComponent,
    CircleMovementComponent,
    SplineCurveComponent,
    SplineFollowComponent,
    LookAtTargetComponent,
    LightComponent,
    ReflectionProbeComponent,
    FlyingPropComponent
>;

class World {
public:
    World(const WorldDesc& desc)
        : _desc(desc)
    {
    }

    World(const World&) = delete;
    World& operator=(const World&) = delete;
    World(World&&) = delete;
    World& operator=(World&&) = delete;

    uint64_t GetRuntimeId() const noexcept { return _runtimeId; }

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
    inline static std::atomic<uint64_t> _nextRuntimeId{1};
    const uint64_t _runtimeId = _nextRuntimeId.fetch_add(1, std::memory_order_relaxed);

    WorldDesc _desc;

    WorldECS _ecs;

    float _timeScale = 1.0f;
    bool _bPaused = false;
};

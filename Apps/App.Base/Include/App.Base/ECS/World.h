#pragma once
#include <string>
#include <filesystem>
#include <cstdint>
#include <functional>

#include "Engine.Core/ECS/ECSStorage.h"
#include "App.Base/ECS/Components/CameraComponent.h"
#include "App.Base/ECS/Components/CircleMovementComponent.h"
#include "App.Base/ECS/Components/LookAtTargetComponent.h"
#include "App.Base/ECS/Components/LightComponent.h"
#include "App.Base/ECS/Components/ReflectionProbeComponent.h"
#include "App.Base/ECS/Components/SkyBoxComponent.h"
#include "App.Base/ECS/Components/SkyAtmosphereComponent.h"
#include "App.Base/ECS/Components/GrassRenderComponent.h"
#include "App.Base/ECS/Components/WindFieldComponent.h"
#include "App.Base/ECS/Components/ParticleEmitterComponent.h"
#include "App.Base/ECS/Components/FluidRenderComponent.h"
#include "App.Base/ECS/Components/NameComponent.h"
#include "App.Base/ECS/Components/SplineCurveComponent.h"
#include "App.Base/ECS/Components/SplineFollowComponent.h"
#include "App.Base/ECS/Components/StaticMeshRenderComponent.h"
#include "App.Base/ECS/Components/SkeletalMeshRenderComponent.h"
#include "App.Base/ECS/Components/TransformComponent.h"
#include "App.Base/ECS/Components/VelocityComponent.h"
#include "App.Base/ECS/Components/RigidBodyComponent.h"
#include "App.Base/ECS/Components/ColliderComponent.h"

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
    SkeletalMeshRenderComponent,
    CameraComponent,
    CircleMovementComponent,
    SplineCurveComponent,
    SplineFollowComponent,
    LookAtTargetComponent,
    LightComponent,
    ReflectionProbeComponent,
    SkyBoxComponent,
    SkyAtmosphereComponent,
    GrassRenderComponent,
    WindFieldComponent,
    ParticleEmitterComponent,
    FluidRenderComponent,
    RigidBodyComponent,
    ColliderComponent
>;

class World {
public:
    World(const WorldDesc& desc)
        : _desc(desc)
    {
    }

    bool Load();

    bool Save(const std::filesystem::path& path);

    void Tick(float dt);

    void SetTimeScale(float scale);
    float GetTimeScale() const;

    void SetPaused(bool paused);
    bool IsPaused() const;

    WorldECS& GetECS();
    const WorldECS& GetECS() const;

    void SetName(const std::string& name);

    const std::filesystem::path& GetPath() const;

    void SetSkySource(SkySource source, Entity entity = InvalidEntity);
    SkySource GetSkySource() const { return _skySource; }
    Entity GetSkyEntity() const { return _skyEntity; }
    uint64_t GetSkyRevision() const { return _skyRevision; }

    Entity ActiveCamera = InvalidEntity;
    // Return true after creating the mesh entities; false keeps the loader's default entity.
    std::function<bool(const std::string&, const StaticMeshRenderComponent&)> SceneMeshFactory;

private:
    friend class App;
    double _physicsAccumulator = 0;

    SkySource _skySource = SkySource::Off;
    Entity _skyEntity = InvalidEntity;
    uint64_t _skyRevision = 0;
    WorldDesc _desc;

    WorldECS _ecs;

    float _timeScale = 1.0f;
    bool _bPaused = false;
};

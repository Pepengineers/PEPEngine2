#include "App.Base/Modules/SceneManagerModule.h"

#include "App.Base/BistroLightPlacements.h"
#include "App.Base/ECS/AppConfigLoader.h"
#include "App.Base/Systems/CircleMovementSystem.h"
#include "App.Base/Systems/MovementSystem.h"
#include "App.Base/Systems/GPUDataUpdateSystem.h"
#include "App.Base/Systems/LookAtTargetSystem.h"
#include "App.Base/Systems/SplineFollowSystem.h"
#include "App.Base/Modules/RenderModule.h"
#include "Engine.Core/BenchmarkEngine.h"
#include "Engine.Core/Logging/Logger.h"
#include <stdexcept>
#include "App.Base/Modules/PhysicsModule.h"
#include "App.Base/Systems/DynamicBodyUpdateSystem.h"
#include "App.Base/Systems/KinematicBodyUpdateSystem.h"

SceneManagerModule::SceneManagerModule(std::filesystem::path scenePath) :
    _scenePath(std::move(scenePath))
{
}

SceneManagerModule::~SceneManagerModule()
{

}

void SceneManagerModule::Initialize()
{
    Uninitialize();
    if (_scenePath.empty()) return;

    const std::filesystem::path& scenePath = _scenePath;

    if (!LoadWorld(scenePath))
    {
        Log::Error("Scene", "Failed to load world: ", scenePath);
        throw std::runtime_error("Failed to load scene: " + scenePath.string());
    }

    World* world = GetWorld(0);
    if (!world)
    {
        Log::Error("Scene", "Loaded scene has no world: ", scenePath);
        throw std::runtime_error("Loaded scene has no world: " + scenePath.string());
    }

    if (scenePath.wstring().find(L"Amazon Lumberyard Bistro") != std::wstring::npos)
    {
        Log::Info("Scene", "Creating Bistro light fixtures");
        BenchmarkEngine::GetLocator().GetModule<RenderModule>()->ReserveTransformCapacity(
            static_cast<UINT>(sizeof(BistroLightPlacements) / sizeof(BistroLightPlacements[0])) + 1);
        const auto create = [world](const char* name, Vector3 position, Vector3 direction,
            LightType type, float range, float intensity)
        {
            direction.Normalize();
            const Vector3 rotation(XMConvertToDegrees(std::asin(-direction.y)), XMConvertToDegrees(std::atan2(direction.x, direction.z)), 0);
            auto entity = world->GetECS().CreateEntity();
            entity.AddComponent<NameComponent>(name);
            entity.AddComponent<TransformComponent>(position, rotation);
            auto& light = entity.AddComponent<LightComponent>();
            light.Type = type;
            light.Range = range;
            light.Intensity = intensity;
            if (type == LightType::Directional)
            {
                entity.AddComponent<SkyBoxComponent>();
                world->SetSkySource(SkySource::SkyBox, entity.GetId());
                light.Color = Vector3(1.0f, 0.95f, 0.875f);
                light.ShadowUpdate = ShadowUpdateMode::EveryFrame;
            }
        };
        create("Sun", Vector3::Zero, Vector3(0.35f, -1.0f, 0.25f), LightType::Directional, 0, 4.0f);
        for (const auto& fixture : BistroLightPlacements)
            create(fixture.Name, fixture.Position, fixture.Direction,
                fixture.Kind == BistroFixtureKind::Point ? LightType::Point : LightType::Spot, fixture.Range, fixture.Intensity);
    }
    Log::Info("Scene", "Scene initialization complete: ", scenePath);
}

void SceneManagerModule::Uninitialize()
{
    for (auto& world : _worldVector)
    {
        if (world)
        {
            OnWorldDestroyed.Broadcast(*world);
        }
    }

    _systemVector.clear();
    _worldVector.clear();
}

bool SceneManagerModule::LoadScene(
    const std::string& scenePath,
    const AppConfig& appConfig)
{
    Logger::Info("SceneManagerModule::LoadScene path:\n{}", scenePath);

    Uninitialize();

    SceneConfig sceneConfig;

    try
    {
        sceneConfig = AppConfigLoader::LoadSceneConfig(scenePath);
    }
    catch (const std::exception& exception)
    {
        Logger::Error("Scene yaml load failed: {}", exception.what());
        return false;
    }

    Logger::Info("SceneManagerModule::LoadScene world count: {}", sceneConfig.Worlds.size());

    for (const SceneWorldConfig& sceneWorldConfig : sceneConfig.Worlds)
    {
        Logger::Info("SceneManagerModule::LoadScene loading world yaml:\n{}", sceneWorldConfig.Path);

        if (!LoadConfiguredWorld(sceneWorldConfig, appConfig))
        {
            return false;
        }
    }

    return true;
}

bool SceneManagerModule::LoadConfiguredWorld(
    const SceneWorldConfig& sceneWorldConfig,
    const AppConfig& appConfig)
{
    Logger::Info("LoadConfiguredWorld sceneWorldConfig.Path:\n{}", sceneWorldConfig.Path);

    WorldConfig worldConfig;

    try
    {
        worldConfig = AppConfigLoader::LoadWorldConfig(sceneWorldConfig.Path);
    }
    catch (const std::exception& exception)
    {
        Logger::Error("World yaml load failed: {}", exception.what());
        return false;
    }

    WorldDesc desc;
    desc.Name = !worldConfig.Name.empty()
        ? worldConfig.Name
        : sceneWorldConfig.Name;
    desc.WorldFilePath = sceneWorldConfig.Path;

    auto world = std::make_unique<World>(desc);
    World* worldPtr = world.get();

    if (!LoadWorldContents(*worldPtr))
    {
        Logger::Error("World::Load failed: {}", desc.WorldFilePath.string());
        return false;
    }

    _worldVector.push_back(std::move(world));

    if (!AddSystemsFromConfig(worldPtr, worldConfig, appConfig))
    {
        UnloadWorld(_worldVector.size() - 1);
        return false;
    }

    return true;
}

bool SceneManagerModule::AddSystemsFromConfig(
    World* world,
    const WorldConfig& worldConfig,
    const AppConfig& appConfig)
{
    if (!world)
    {
        return false;
    }

    std::vector<SystemConfig> systems = worldConfig.Systems;

    std::stable_sort(
        systems.begin(),
        systems.end(),
        [](const SystemConfig& a, const SystemConfig& b)
        {
            return a.Priority < b.Priority;
        }
    );

    for (const SystemConfig& systemConfig : systems)
    {
        if (appConfig.IsSystemBanned(systemConfig.Name))
        {
            continue;
        }

        if (!AddSystemByName(world, systemConfig))
        {
            return false;
        }
    }

    return true;
}

bool SceneManagerModule::AddSystemByName(
    World* world,
    const SystemConfig& systemConfig)
{
    if (!world)
    {
        return false;
    }

    if (systemConfig.Name.empty())
    {
        return false;
    }

    const uint8_t priority = ToSystemPriority(systemConfig.Priority);

    if (systemConfig.Name == "MovementSystem")
    {
        AddSystem<MovementSystem>(world, priority);
        return true;
    }

    if (systemConfig.Name == "FixedMovementSystem")
    {
        AddSystem<MovementSystem>(world, priority, SystemUpdatePhase::FixedPrePhysics);
        return true;
    }

    if (systemConfig.Name == "CircleMovementSystem")
    {
        AddSystem<CircleMovementSystem>(world, priority);
        return true;
    }

    if (systemConfig.Name == "SplineFollowSystem")
    {
        AddSystem<SplineFollowSystem>(world, priority);
        return true;
    }

    if (systemConfig.Name == "FixedSplineFollowSystem")
    {
        AddSystem<SplineFollowSystem>(world, priority, SplineUpdateMode::Fixed);
        return true;
    }

    if (systemConfig.Name == "LookAtTargetSystem")
    {
        AddSystem<LookAtTargetSystem>(world, priority);
        return true;
    }

    if (systemConfig.Name == "DynamicBodyUpdateSystem")
    {
        auto physics = BenchmarkEngine::GetLocator().GetModule<PhysicsModule>();
        if (!physics) return false;
        AddSystem<DynamicBodyUpdateSystem>(world, priority, *physics);
        return true;
    }

    if (systemConfig.Name == "KinematicBodyUpdateSystem")
    {
        auto physics = BenchmarkEngine::GetLocator().GetModule<PhysicsModule>();
        if (!physics) return false;
        AddSystem<KinematicBodyUpdateSystem>(world, priority, *physics);
        return true;
    }

    if (systemConfig.Name == "GPUDataUpdateSystem")
    {
        AddSystem<GPUDataUpdateSystem>(world, priority);
        return true;
    }

    Logger::Error("Unknown system name: {}", systemConfig.Name);
    return false;
}

uint8_t SceneManagerModule::ToSystemPriority(int priority)
{
    if (priority < 0)
    {
        return 0;
    }

    if (priority > 255)
    {
        return 255;
    }

    return static_cast<uint8_t>(priority);
}

bool SceneManagerModule::LoadWorld(const std::filesystem::path& path)
{
    Log::Info("Scene", "Loading world: ", path);
    WorldDesc desc;
    desc.WorldFilePath = path;

    auto world = std::make_unique<World>(desc);

    if (!LoadWorldContents(*world)) return false;

    _worldVector.push_back(std::move(world));
    return true;
}

bool SceneManagerModule::LoadWorldContents(World& world)
{
    // Reserve ownership before notifying modules that keep references to the world.
    _worldVector.reserve(_worldVector.size() + 1);
    try
    {
        OnWorldCreated.Broadcast(world);
        if (world.Load()) return true;
    }
    catch (...)
    {
        OnWorldDestroyed.Broadcast(world);
        throw;
    }
    OnWorldDestroyed.Broadcast(world);
    return false;
}

bool SceneManagerModule::UnloadWorld(size_t index)
{
    if (index >= _worldVector.size()) { return false; }

    World* worldToRemove = _worldVector[index].get();
    _systemVector.erase(std::remove_if(_systemVector.begin(), _systemVector.end(),
            [worldToRemove](const SystemEntry& entry)
            {
                return entry.world == worldToRemove;
            }
        ),
        _systemVector.end()
    );

    OnWorldDestroyed.Broadcast(*worldToRemove);

    _worldVector.erase(_worldVector.begin() + index);
    return true;
}

bool SceneManagerModule::SaveWorld(World* world, const std::filesystem::path& path)
{
    if (!world)
    {
        return false;
    }

    return world->Save(path);
}

World* SceneManagerModule::GetWorld(size_t index)
{
    if (index >= _worldVector.size())
    {
        return nullptr;
    }

    return _worldVector[index].get();
}

size_t SceneManagerModule::GetWorldCount() const
{
    return _worldVector.size();
}

void SceneManagerModule::UpdateSystems(JobSystem& jobs, World& world, float dt, SystemUpdatePhase phase)
{
    JobHandle work;
    try
    {
        for (auto& entry : _systemVector)
            if (entry.world == &world && entry.system->GetUpdatePhase() == phase)
                work = entry.system->Schedule(jobs, world, dt, work);
        work.Complete();
    }
    catch (...)
    {
        try { work.Complete(); } catch (...) {}
        throw;
    }
}

void SceneManagerModule::OnUpdate()
{
}

void SceneManagerModule::OnRender()
{
}

bool SceneManagerModule::ShouldTick()
{
    return false;
}

bool SceneManagerModule::ShouldRender()
{
    return false;
}

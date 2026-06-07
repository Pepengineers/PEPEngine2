#include "App.Base/Modules/SceneManagerModule.h"

#include "App.Base/AppConfigLoader.h"

#include "App.Base/Systems/CircleMovementSystem.h"
#include "App.Base/Systems/MovementSystem.h"
#include "App.Base/Systems/RenderSubmitSystem.h"
#include "App.Base/Systems/GPUDataUpdateSystem.h"
#include "App.Base/Systems/LookAtTargetSystem.h"
#include "App.Base/Systems/SplineFollowSystem.h"
#include "Engine.Core/System.h"

#include <exception>
#include <string>
#include <Windows.h>

namespace
{
    void ShowDebugMessage(const std::string& title, const std::string& message)
    {
        //todo spdlog here
        //MessageBoxA(nullptr, message.c_str(), title.c_str(), MB_OK);
    }
}

SceneManagerModule::SceneManagerModule(GameTimer* timer)
    : _timer(timer)
{
}

SceneManagerModule::~SceneManagerModule()
{
    
}

void SceneManagerModule::Initialize()
{
    
}

void SceneManagerModule::Uninitialize()
{
    _systemVector.clear();

    for (auto& world : _worldVector)
    {
        if (world)
        {
            OnWorldDestroyed.Broadcast(*world);
        }
    }

    _worldVector.clear();
}

bool SceneManagerModule::LoadScene(
    const std::string& scenePath,
    const AppConfig& appConfig)
{
    ShowDebugMessage("SceneManagerModule::LoadScene", "scenePath:\n" + scenePath);

    Uninitialize();

    SceneConfig sceneConfig;

    try
    {
        sceneConfig = AppConfigLoader::LoadSceneConfig(scenePath);
    }
    catch (const std::exception& exception)
    {
        ShowDebugMessage("Scene yaml load failed", exception.what());
        return false;
    }

    ShowDebugMessage(
        "SceneManagerModule::LoadScene",
        "World count: " + std::to_string(sceneConfig.Worlds.size()));

    std::vector<SceneWorldConfig> worlds = sceneConfig.Worlds;

    for (const SceneWorldConfig& sceneWorldConfig : worlds)
    {
        ShowDebugMessage(
            "SceneManagerModule::LoadScene",
            "Loading world yaml:\n" + sceneWorldConfig.Path);

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
    ShowDebugMessage(
        "LoadConfiguredWorld",
        "sceneWorldConfig.Path:\n" + sceneWorldConfig.Path);

    WorldConfig worldConfig;

    try
    {
        worldConfig = AppConfigLoader::LoadWorldConfig(sceneWorldConfig.Path);
    }
    catch (const std::exception& exception)
    {
        ShowDebugMessage("World yaml load failed", exception.what());
        return false;
    }

    ShowDebugMessage(
        "LoadConfiguredWorld",
        "worldConfig.SourcePath:\n" + worldConfig.SourcePath);

    WorldDesc desc;
    desc.Name = !worldConfig.Name.empty()
        ? worldConfig.Name
        : sceneWorldConfig.Name;

    desc.WorldFilePath = !worldConfig.SourcePath.empty()
        ? std::filesystem::path(worldConfig.SourcePath)
        : std::filesystem::path(sceneWorldConfig.Path);

    auto world = std::make_unique<World>(desc);
    World* worldPtr = world.get();

    OnWorldCreated.Broadcast(*worldPtr);

    if (!world->Load())
    {
        ShowDebugMessage(
            "World::Load failed",
            desc.WorldFilePath.string());
        OnWorldDestroyed.Broadcast(*worldPtr);
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
        if (!AddSystemByName(world, systemConfig, appConfig))
        {
            return false;
        }
    }

    return true;
}

bool SceneManagerModule::AddSystemByName(
    World* world,
    const SystemConfig& systemConfig,
    const AppConfig& appConfig)
{
    if (!world)
    {
        return false;
    }

    if (systemConfig.Name.empty())
    {
        return false;
    }

    if (appConfig.IsSystemBanned(systemConfig.Name))
    {
        return true;
    }

    const uint8_t priority = ToSystemPriority(systemConfig.Priority);

    if (systemConfig.Name == "MovementSystem")
    {
        AddSystem<MovementSystem>(world, priority);
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

    if (systemConfig.Name == "LookAtTargetSystem")
    {
        AddSystem<LookAtTargetSystem>(world, priority);
        return true;
    }

    if (systemConfig.Name == "GPUDataUpdateSystem")
    {
        AddSystem<GPUDataUpdateSystem>(world, priority);
        return true;
    }

    if (systemConfig.Name == "RenderSubmitSystem")
    {
        AddSystem<RenderSubmitSystem>(world, priority);
        return true;
    }

    ShowDebugMessage(
        "Unknown system name",
        systemConfig.Name);
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
    WorldDesc desc;
    desc.WorldFilePath = path;

    auto world = std::make_unique<World>(desc);

    World& worldRef = *world;
    OnWorldCreated.Broadcast(worldRef);

    if (!world->Load())
    {
        OnWorldDestroyed.Broadcast(worldRef);
        return false;
    }

    _worldVector.push_back(std::move(world));
    return true;
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

void SceneManagerModule::OnUpdate()
{
    Tick(_timer->DeltaTime());
}

void SceneManagerModule::OnRender()
{
}

bool SceneManagerModule::ShouldTick()
{
    return true;
}

bool SceneManagerModule::ShouldRender()
{
    return false;
}

void SceneManagerModule::Tick(float dt)
{
    for (auto& entry : _systemVector)
    {
        if (!entry.world || entry.world->IsPaused())
        {
            continue;
        }

        entry.system->Tick(*entry.world, dt);
    }
}

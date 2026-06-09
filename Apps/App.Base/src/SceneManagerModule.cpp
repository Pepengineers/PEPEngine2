#include "App.Base/Modules/SceneManagerModule.h"

#include "App.Base/Systems/CircleMovementSystem.h"
#include "App.Base/Systems/MovementSystem.h"
#include "App.Base/Systems/RenderSubmitSystem.h"
#include "App.Base/Systems/GPUDataUpdateSystem.h"
#include "App.Base/Systems/LookAtTargetSystem.h"
#include "App.Base/Systems/SplineFollowSystem.h"
#include "Engine.Core/System.h"

SceneManagerModule::SceneManagerModule(GameTimer* timer) :
    _timer(timer)
{
    
}

SceneManagerModule::~SceneManagerModule()
{
    
}

// todo implement world loading from file
void SceneManagerModule::Initialize()
{
    Uninitialize();

    //if (!LoadWorld("world1.yaml"))
    //{
    //    // todo runtime error or log
    //}

    /*const std::filesystem::path scenePath =
    std::filesystem::path(ASSETS_FOLDER) /
    "Scenes" /
    "Gallery";*/

    const std::filesystem::path scenePath =
    std::filesystem::path(ASSETS_FOLDER) /
    "Scenes" /
    "Amazon Lumberyard Bistro" /
    "Interior" /
    "interior.obj";

    if (!LoadWorld(scenePath))
    {
        // todo runtime error or log 
    }

    World* world = GetWorld(0);
    if (!world)
    {
        // todo runtime error or log 
    }

    AddSystem<MovementSystem>(world, 0);
    AddSystem<CircleMovementSystem>(world, 1);
    AddSystem<SplineFollowSystem>(world, 2);
    AddSystem<LookAtTargetSystem>(world, 3);
    AddSystem<GPUDataUpdateSystem>(world, 101);
    AddSystem<RenderSubmitSystem>(world, 102);
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
            }),
        _systemVector.end());

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

#include "App.Base/Modules/SceneManagerModule.h"

#include "App.Base/BistroLightPlacements.h"
#include "App.Base/Modules/RenderModule.h"
#include "Engine.Core/BenchmarkEngine.h"
#include "Engine.Core/Logging/Logger.h"
#include <stdexcept>

SceneManagerModule::SceneManagerModule(std::filesystem::path scenePath) :
    _scenePath(std::move(scenePath))
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

    const std::filesystem::path scenePath = _scenePath.empty() ?
    std::filesystem::path(ASSETS_FOLDER) /
    "Scenes" /
    "Amazon Lumberyard Bistro" /
    "Interior" /
    "interior.obj" : _scenePath;

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
                light.Color = Vector3(1.0f, 0.95f, 0.875f);
                light.ShadowUpdate = ShadowUpdateMode::EveryFrame;
            }
            return entity;
        };
        auto sun = create("Sun", Vector3::Zero, Vector3(0.35f, -1.0f, 0.25f), LightType::Directional, 0, 4.0f);
        sun.AddComponent<SkyBoxComponent>();
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

    _worldVector.clear();
}

bool SceneManagerModule::LoadWorld(const std::filesystem::path& path)
{
    Log::Info("Scene", "Loading world: ", path);
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

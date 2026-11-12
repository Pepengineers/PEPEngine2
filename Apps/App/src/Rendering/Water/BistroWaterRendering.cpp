#include "Rendering/Water/BistroWaterRendering.h"

BistroWaterRendering::BistroWaterRendering(std::shared_ptr<SceneManagerModule> scenes) : _scenes(std::move(scenes))
{
    _created = _scenes->OnWorldCreated.AddListener([this](World& world) { Subscribe(world); });
    _destroyed = _scenes->OnWorldDestroyed.AddListener([this](World& world) { Unsubscribe(world); });
    for (size_t i = 0; i < _scenes->GetWorldCount(); ++i) Subscribe(*_scenes->GetWorld(i));
}

BistroWaterRendering::~BistroWaterRendering()
{
    _scenes->OnWorldCreated.RemoveListener(_created);
    _scenes->OnWorldDestroyed.RemoveListener(_destroyed);
    while (!_worlds.empty()) Unsubscribe(*_worlds.back().Scene);
}

void BistroWaterRendering::Subscribe(World& world)
{
    if (std::any_of(_worlds.begin(), _worlds.end(), [&](const auto& item) { return item.Scene == &world; })) return;
    RegisterBistroWaterComponents(world);
    auto& pool = world.GetECS().GetPool<FluidRenderComponent>();
    const auto own = [this](Entity, FluidRenderComponent& component) { component._renderOwner = ++_owner; };
    _worlds.push_back({ &world, pool.OnComponentCreated.AddListener(own), pool.OnComponentUpdated.AddListener(own) });
    world.GetECS().ForEach<FluidRenderComponent>(own);
}

void BistroWaterRendering::Unsubscribe(World& world)
{
    const auto found = std::find_if(_worlds.begin(), _worlds.end(), [&](const auto& item) { return item.Scene == &world; });
    if (found == _worlds.end()) return;
    auto& pool = world.GetECS().GetPool<FluidRenderComponent>();
    pool.OnComponentCreated.RemoveListener(found->Created);
    pool.OnComponentUpdated.RemoveListener(found->Updated);
    _worlds.erase(found);
}

std::vector<GDX12FluidInstance> BistroWaterRendering::CollectScene(float deltaSeconds)
{
    std::vector<GDX12FluidInstance> instances;
    for (const auto& subscription : _worlds)
    {
        auto& world = *subscription.Scene;
        auto& ecs = world.GetECS();
        ecs.ForEach<FluidRenderComponent, TransformComponent>([&](Entity, FluidRenderComponent& component, const TransformComponent& transform)
        {
            GDX12FluidInstance instance;
            instance.Id = component._renderOwner;
            instance.Component = component;
            instance.ContainerToWorld = Matrix::CreateScale(transform.Scale) * Matrix::CreateFromYawPitchRoll(
                XMConvertToRadians(transform.Rotation.y), XMConvertToRadians(transform.Rotation.x),
                XMConvertToRadians(transform.Rotation.z)) * Matrix::CreateTranslation(transform.Location);
            instance.ClockScale = world.IsPaused() ? 0 : world.GetTimeScale();
            instance.DeltaSeconds = deltaSeconds * instance.ClockScale;
            ValidateFluidInstance(instance);
            instances.push_back(std::move(instance));
        });
    }
    return instances;
}

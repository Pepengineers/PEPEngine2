#include "Rendering/Grass/BistroGrassGeometry.h"
#include "App.Base/Modules/RenderModule.h"

BistroGrassGeometry::BistroGrassGeometry(std::shared_ptr<SceneManagerModule> scenes, RenderModule& renderer)
    : _scenes(std::move(scenes)), _renderer(renderer)
{
    _created = _scenes->OnWorldCreated.AddListener([this](World& world) { Subscribe(world); });
    _destroyed = _scenes->OnWorldDestroyed.AddListener([this](World& world) { Unsubscribe(world); });
    for (size_t i = 0; i < _scenes->GetWorldCount(); ++i) Subscribe(*_scenes->GetWorld(i));
}

BistroGrassGeometry::~BistroGrassGeometry()
{
    _scenes->OnWorldCreated.RemoveListener(_created);
    _scenes->OnWorldDestroyed.RemoveListener(_destroyed);
    while (!_subscriptions.empty()) Unsubscribe(*_subscriptions.begin()->first);
}

void BistroGrassGeometry::Subscribe(World& world)
{
    if (_subscriptions.count(&world)) return;
    RegisterBistroGrassComponents(world);
    auto& ecs = world.GetECS();
    auto& grass = ecs.GetPool<GrassRenderComponent>();
    auto& wind = ecs.GetPool<WindFieldComponent>();
    const auto ownGrass = [this](Entity, GrassRenderComponent& component) { component._renderOwner = ++_owner; };
    const auto ownWind = [this](Entity, WindFieldComponent& component) { component._renderOwner = ++_owner; };
    _subscriptions.emplace(&world, Subscriptions{
        grass.OnComponentCreated.AddListener(ownGrass), grass.OnComponentUpdated.AddListener(ownGrass),
        wind.OnComponentCreated.AddListener(ownWind), wind.OnComponentUpdated.AddListener(ownWind) });
    ecs.ForEach<GrassRenderComponent>(ownGrass);
    ecs.ForEach<WindFieldComponent>(ownWind);
}

void BistroGrassGeometry::Unsubscribe(World& world)
{
    const auto found = _subscriptions.find(&world);
    if (found == _subscriptions.end()) return;
    const auto listeners = found->second;
    auto& grass = world.GetECS().GetPool<GrassRenderComponent>();
    auto& wind = world.GetECS().GetPool<WindFieldComponent>();
    grass.OnComponentCreated.RemoveListener(listeners.GrassCreated);
    grass.OnComponentUpdated.RemoveListener(listeners.GrassUpdated);
    wind.OnComponentCreated.RemoveListener(listeners.WindCreated);
    wind.OnComponentUpdated.RemoveListener(listeners.WindUpdated);
    _subscriptions.erase(found);
    _grass.clear();
    _wind.clear();
}

void BistroGrassGeometry::CollectScene(float deltaSeconds)
{
    _grass.clear();
    _wind.clear();
    const auto controls = GetGrassWindControls();
    auto& scenes = _scenes;
    if (controls.GrassEnabled && controls.GrassQuality > 0)
    {
        for (size_t worldIndex = 0; worldIndex < scenes->GetWorldCount(); ++worldIndex)
        {
            auto* world = scenes->GetWorld(worldIndex);
            auto& ecs = world->GetECS();
            ecs.ForEach<GrassRenderComponent>([&](Entity entity, GrassRenderComponent& component)
            {
                if (!component.Enabled || !component.Material || !ecs.Has<TransformComponent>(entity)) return;
                if (!IsValidGrassSettings(component)) throw std::invalid_argument("Invalid GrassRenderComponent");
                if (!GrassBladeCount(ResolveGrassSettings(component, controls.GrassQuality))) return;
                if (!_renderer.OwnsMaterial(component.Material) || component.Material->Type == MaterialType::Transparent)
                    throw std::invalid_argument("Grass requires an owned opaque or masked material");
                if (!component._renderOwner) component._renderOwner = ++_owner;
                const auto& transform = ecs.Get<TransformComponent>(entity);
                GDX12GrassInstance instance;
                static_cast<GDX12GrassSettings&>(instance) = component;
                instance.Id = component._renderOwner;
                instance.World = Matrix::CreateScale(transform.Scale) * Matrix::CreateFromYawPitchRoll(
                    XMConvertToRadians(transform.Rotation.y), XMConvertToRadians(transform.Rotation.x),
                    XMConvertToRadians(transform.Rotation.z)) * Matrix::CreateTranslation(transform.Location);
                instance.MaterialIndex = component.Material->_CBufferIndex;
                instance.ResetRevision = component.ResetRevision;
                instance.MaterialRevision = component.Material->GetRevision();
                if (component.WindEntity != InvalidEntity && ecs.Has<WindFieldComponent>(component.WindEntity) &&
                    ecs.Has<TransformComponent>(component.WindEntity))
                {
                    auto& field = ecs.Get<WindFieldComponent>(component.WindEntity);
                    if (field.Enabled && controls.WindEnabled && controls.WindQuality > 0)
                    {
                        if (!field._renderOwner) field._renderOwner = ++_owner;
                        instance.WindId = field._renderOwner;
                        const auto found = std::find_if(_wind.begin(), _wind.end(), [&](const auto& item) { return item.Id == instance.WindId; });
                        if (found == _wind.end())
                        {
                            GDX12WindInstance snapshot;
                            static_cast<GDX12WindSettings&>(snapshot) = field;
                            snapshot.Id = field._renderOwner;
                            const auto& location = ecs.Get<TransformComponent>(component.WindEntity).Location;
                            snapshot.CenterCm = Vector2(location.x, location.z);
                            snapshot.DeltaSeconds = field.Paused || world->IsPaused() ? 0 :
                                deltaSeconds * world->GetTimeScale();
                            _wind.push_back(std::move(snapshot));
                        }
                    }
                }
                _grass.push_back(std::move(instance));
            });
        }
    }
    _controls = controls;
}

void BistroGrassGeometry::PrepareFrame(GDX12DeviceResources& resources, const RenderPipelineCommonData& common)
{
    if (!_grass.empty() && !_gpu) _gpu = std::make_unique<GDX12GrassRenderer>(&resources);
    if (!_gpu) return;
    _gpu->BeginFrame(common, _grass, _wind, _controls);
    _gpu->PublishPrimitives(resources.Primitives, this);
}

void BistroGrassGeometry::PrepareGPU(GDX12CommandList* commands)
{ if (_gpu) _gpu->PrepareGPU(commands); }

void BistroGrassGeometry::PrepareView(const GDX12GeometryContext& view, UINT patch)
{ if (_gpu) _gpu->PrepareView(view, patch); }

void BistroGrassGeometry::Expand(GDX12CommandList* commands, const GDX12GeometryContext& view, UINT patch)
{ if (_gpu) _gpu->Expand(commands, view, patch); }

void BistroGrassGeometry::GatherBatches(const GDX12GeometryContext& view, std::vector<GDX12RenderBatch>& batches) const
{ if (_gpu) _gpu->GatherBatches(view, batches); }

std::vector<GDX12RenderGraph::BufferAccess> BistroGrassGeometry::GetGeometryBufferAccesses(GDX12GeometryStage) const
{ return _gpu ? _gpu->DrawAccesses() : std::vector<GDX12RenderGraph::BufferAccess>{}; }

std::vector<GDX12RenderGraph::BufferAccess> BistroGrassGeometry::PreparationAccesses() const
{ return _gpu ? _gpu->PreparationAccesses() : std::vector<GDX12RenderGraph::BufferAccess>{}; }

std::vector<GDX12RenderGraph::BufferAccess> BistroGrassGeometry::ExpansionAccesses(const GDX12GeometryContext& view, UINT patch) const
{ return _gpu ? _gpu->ExpansionAccesses(view, patch) : std::vector<GDX12RenderGraph::BufferAccess>{}; }

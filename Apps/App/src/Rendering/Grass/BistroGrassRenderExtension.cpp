#include "Rendering/Grass/BistroGrassRenderExtension.h"
#include "App.Base/Modules/RenderModule.h"

BistroGrassRenderExtension::BistroGrassRenderExtension(std::shared_ptr<SceneManagerModule> scenes, RenderModule& renderer)
    : _scenes(std::move(scenes)), _renderer(renderer)
{
    _created = _scenes->OnWorldCreated.AddListener([this](World& world) { Subscribe(world); });
    _destroyed = _scenes->OnWorldDestroyed.AddListener([this](World& world) { Unsubscribe(world); });
    for (size_t i = 0; i < _scenes->GetWorldCount(); ++i) Subscribe(*_scenes->GetWorld(i));
}

BistroGrassRenderExtension::~BistroGrassRenderExtension()
{
    _scenes->OnWorldCreated.RemoveListener(_created);
    _scenes->OnWorldDestroyed.RemoveListener(_destroyed);
    while (!_subscriptions.empty()) Unsubscribe(*_subscriptions.begin()->first);
}

void BistroGrassRenderExtension::Subscribe(World& world)
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

void BistroGrassRenderExtension::Unsubscribe(World& world)
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

void BistroGrassRenderExtension::CollectScene(float deltaSeconds)
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

void BistroGrassRenderExtension::PrepareFrame(GDX12CommandList* commands, GDX12DeviceResources& resources,
    const RenderPipelineCommonData& common)
{
    if (!_grass.empty() && !_gpu) _gpu = std::make_unique<GDX12GrassRenderer>(&resources);
    if (_gpu) _gpu->BeginFrame(commands, common, _grass, _wind, _controls);
}

void BistroGrassRenderExtension::AppendSceneContributions(GDX12SceneContributions& scene) const
{
    BoundingBox bounds;
    if (_gpu && _gpu->GetBounds(bounds)) scene.Append(bounds, _gpu->Revision(), true, _gpu->HasCasters());
}

void BistroGrassRenderExtension::DeclareGeometryResources(GDX12RenderGraphBuilder& graph) const
{
    // shortcut: direct-queue scratch is one mutable set; declare physical buffers before cross-queue geometry.
    graph.ReadWriteExternal(this, "Application geometry scratch");
}

void BistroGrassRenderExtension::DrawGeometry(GDX12CommandList* commands, const GDX12GeometryContext& context)
{
    if (!_gpu || context.StaticOnly) return;
    GDX12GrassRenderer::Stage stage;
    switch (context.Stage)
    {
    case GDX12GeometryStage::Depth: stage = GDX12GrassRenderer::Stage::Depth; break;
    case GDX12GeometryStage::DepthNormals: stage = GDX12GrassRenderer::Stage::PrepassNormals; break;
    case GDX12GeometryStage::GBuffer: stage = GDX12GrassRenderer::Stage::GBuffer; break;
    case GDX12GeometryStage::Forward: stage = GDX12GrassRenderer::Stage::Full; break;
    case GDX12GeometryStage::Shadow: stage = GDX12GrassRenderer::Stage::Shadow; break;
    case GDX12GeometryStage::ReflectionCapture: stage = GDX12GrassRenderer::Stage::Capture; break;
    default: return;
    }
    GDX12GrassRenderer::CaptureLighting lighting;
    if (context.CaptureLighting)
    {
        lighting.SunShadow = context.CaptureLighting->SunShadow;
        lighting.Irradiance = context.CaptureLighting->Irradiance;
        lighting.Prefiltered = context.CaptureLighting->Prefiltered;
        lighting.BRDF = context.CaptureLighting->BRDF;
        lighting.LocalShadows = context.CaptureLighting->LocalShadows;
        lighting.ShadowConstants = context.CaptureLighting->ShadowConstants;
    }
    _gpu->Draw(commands, stage, context.ViewProjection, context.Eye, context.CameraConstants,
        context.CaptureLighting ? &lighting : nullptr);
}

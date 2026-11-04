#pragma once
#include "Engine.RendererDX12/GDX12RenderExtension.h"
#include "Rendering/Grass/GDX12GrassRenderer.h"
#include "Components/BistroGrassComponents.h"
#include "App.Base/Modules/SceneManagerModule.h"
#include <map>

class RenderModule;

class BistroGrassRenderExtension final : public GDX12RenderExtension
{
public:
    BistroGrassRenderExtension(std::shared_ptr<SceneManagerModule> scenes, RenderModule& renderer);
    ~BistroGrassRenderExtension() override;
    void CollectScene(float deltaSeconds);
    void PrepareFrame(GDX12CommandList*, GDX12DeviceResources&, const RenderPipelineCommonData&) override;
    void AppendSceneContributions(GDX12SceneContributions&) const override;
    void DeclareGeometryResources(GDX12RenderGraphBuilder&) const override;
    void DrawGeometry(GDX12CommandList*, const GDX12GeometryContext&) override;

private:
    void Subscribe(World& world);
    void Unsubscribe(World& world);
    struct Subscriptions { ListenerHandle GrassCreated, GrassUpdated, WindCreated, WindUpdated; };
    std::shared_ptr<SceneManagerModule> _scenes;
    RenderModule& _renderer;
    ListenerHandle _created = 0, _destroyed = 0;
    std::map<World*, Subscriptions> _subscriptions;
    uint64_t _owner = 0;
    std::vector<GDX12GrassInstance> _grass;
    std::vector<GDX12WindInstance> _wind;
    GDX12GrassWindControls _controls = {};
    std::unique_ptr<GDX12GrassRenderer> _gpu;
};

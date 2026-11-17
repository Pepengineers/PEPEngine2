#pragma once
#include "Components/BistroInteractionSource.h"
#include "Engine.RendererDX12/GDX12RenderGeometrySource.h"
#include "Rendering/Grass/GDX12GrassWind.h"
#include "Components/BistroGrassComponents.h"
#include "App.Base/Modules/SceneManagerModule.h"
#include <map>

class RenderModule;
namespace BistroRender { class SceneGrass; }

class BistroGrassGeometry final : public IRenderGeometrySource
{
public:
    BistroGrassGeometry(std::shared_ptr<SceneManagerModule> scenes, RenderModule& renderer);
    ~BistroGrassGeometry() override;
    void CollectScene(float deltaSeconds, bool advanceWhilePaused = false);

private:
    friend class BistroRender::SceneGrass;
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
    std::vector<BistroInteractionSource> _sources;
    float _deltaSeconds = 0;
};

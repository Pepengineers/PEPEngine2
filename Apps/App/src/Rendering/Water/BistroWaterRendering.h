#pragma once
#include "App.Base/Modules/SceneManagerModule.h"
#include "Components/BistroWaterComponents.h"
#include "Rendering/Water/GDX12FluidPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12WBOITTransparencyPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12WBOITCompositionPass.h"

class BistroWaterRendering final
{
public:
    explicit BistroWaterRendering(std::shared_ptr<SceneManagerModule> scenes);
    ~BistroWaterRendering();
    std::vector<GDX12FluidInstance> CollectScene(float deltaSeconds);

    GDX12FluidSurfacePass Surface;
    GDX12FluidResolvePass Resolve{ &Surface };
    GDX12WBOITTransparencyPass Transparency{ GDX12WBOITTransparencyPass::DepthPartition::Far };
    GDX12WBOITCompositionPass Composition;

private:
    void Subscribe(World&);
    void Unsubscribe(World&);
    struct Subscription { World* Scene; ListenerHandle Created, Updated; };
    std::shared_ptr<SceneManagerModule> _scenes;
    ListenerHandle _created = 0, _destroyed = 0;
    std::vector<Subscription> _worlds;
    uint64_t _owner = 0;
};

#pragma once
#include "NativeWork.h"
#include "App.Base/Rendering/IRenderPipeline.h"
#include "Engine.RendererDX12/RenderPasses/GDX12DepthOfFieldPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12MotionBlurPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12ReconstructionPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12OutputToScreenPass.h"
#include "Engine.UI/OffscreenUI.h"
#include "Engine.UI/UIScreenComposite.h"
#include "Rendering/UI/BistroUIPosters.h"
#include <array>
#ifdef _DEBUG
#include "Rendering/BistroCollisionPass.h"
#endif

namespace Meta
{
class ScenePostUI
{
public:
    explicit ScenePostUI(RenderPipelineContext&);
    ~ScenePostUI();
    GDX12ReconstructionPass& Reconstruction() { return _reconstruction; }
    std::vector<GDX12RenderPass*> Passes() { return {&_dof, &_blur, &_reconstruction, &_output}; }
    void BeginPlan() { _work.Commands.clear(); }
    void Submitted(UINT slot) { _exports.at(slot) = _pendingExports; }
    bool AllowsFrameGeneration() const;
    void UpdateSettings();
    void PrepareMaterials();
    void Resize(); // Caller drains the executor and the presentation queue first.
    void Reset();
    void Append(Graph&, std::vector<ID3D12Resource*>&, GDX12Texture* hdr,
        GDX12Texture* depth, GDX12Texture* motion, GDX12Texture* featureMotion,
        GDX12Texture* target, const std::function<void(GDX12CommandList*)>& finish);
    void AppendPosters(Graph&, std::vector<ID3D12Resource*>&);
    bool Record(Id id, ID3D12GraphicsCommandList* cmd) const { return _work.Record(id, cmd); }
private:
    Id Packet(Graph&, std::vector<ID3D12Resource*>&, const char*,
        std::vector<std::pair<GDX12Resource*, Mode>>, const std::function<void(GDX12CommandList*)>&,
        std::vector<Id> dependencies = {}, GDX12Texture* renderTarget = nullptr,
        State textureState = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    struct ScreenSlot
    {
        std::unique_ptr<GDX12Texture> Target;
        std::shared_ptr<const Engine::UI::DrawData> Draw;
        std::shared_ptr<const void> RasterOwner, CompositeOwner;
    };
    GDX12DeviceResources& _resources;
    RenderPipelineCommonData& _common;
    GDX12SwapChain* _swapchain;
    GDX12DepthOfFieldPass _dof;
    GDX12MotionBlurPass _blur;
    GDX12ReconstructionPass _reconstruction;
    GDX12OutputToScreenPass _output;
    std::unique_ptr<BistroUIPosters> _posters;
    std::unique_ptr<Engine::UI::OffscreenRenderer> _screen;
    std::unique_ptr<Engine::UI::ScreenComposite> _composite;
    std::vector<ScreenSlot> _slots;
    NativeWork _work;
    std::vector<std::array<GDX12ResourceHandle, 2>> _exports;
#ifdef _DEBUG
    std::unique_ptr<BistroCollisionPass> _collision;
    bool _collisionInitialized = false, _collisionActive = false;
#endif
    std::array<GDX12ResourceHandle, 2> _pendingExports;
    bool _initialized = false, _resize = false, _outputInitialized = false;
};
}

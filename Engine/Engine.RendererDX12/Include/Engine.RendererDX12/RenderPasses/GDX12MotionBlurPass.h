#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

// Inputs: linear HDR, reverse-Z depth, unjittered prevNDC-currentNDC motion.
// Outputs: linear HDR at the input grid, complete NDC motion (including sky/particle coverage).
namespace BistroRender { class ScenePostUI; }

class GDX12MotionBlurPass final : public GDX12RenderPass
{
public:

    GDX12MotionBlurPass();
    // Set before Initialize; the scene graph retires users before resizing.
    void SetPassthroughWhenDisabled(bool enabled = true) { _passthroughWhenDisabled = enabled; }
    bool NeedsTileResize() const;
    void Initialize() override;
    void PrepareRecording(bool completeMotionRequired = true); // Owner-only, after required retirement.
    void RecordCommands(GDX12CommandList* commands);
    void RecordCommands(GDX12CommandList* commands, GDX12Texture* featureMotion);
    void Resize() override;
    void ClearDenendencies() override;

private:
    friend class BistroRender::ScenePostUI;
    void ResizeTiles(UINT tileSize);
    std::unique_ptr<GDX12Texture> _color, _motion, _velocity, _tileMax, _neighborMax;
    std::unique_ptr<GDX12RootSignature> _root;
    ComPtr<ID3D12PipelineState> _prepare, _tiles, _neighbors, _gather;
    UINT _tileSize = 0;
    bool _wasEnabled = false;
    bool _passthroughWhenDisabled = false, _prepareMotion = true, _preparedBlur = false;
};

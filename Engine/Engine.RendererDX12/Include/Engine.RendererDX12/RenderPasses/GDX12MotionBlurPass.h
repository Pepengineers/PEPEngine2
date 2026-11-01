#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

// Inputs: linear HDR, reverse-Z depth, unjittered prevNDC-currentNDC motion.
// Outputs: linear HDR at the input grid, complete NDC motion (including sky).
class GDX12MotionBlurPass final : public GDX12RenderPass
{
public:
    GDX12MotionBlurPass();
    void Initialize() override;
    void Execute(GDX12CommandList* commands) override;
    void Resize() override;
    void ClearDenendencies() override;

private:
    void ResizeTiles(UINT tileSize);
    std::unique_ptr<GDX12Texture> _color, _motion, _velocity, _tileMax, _neighborMax;
    std::unique_ptr<GDX12RootSignature> _root;
    ComPtr<ID3D12PipelineState> _prepare, _tiles, _neighbors, _gather;
    UINT _tileSize = 0;
    bool _wasEnabled = false;
};

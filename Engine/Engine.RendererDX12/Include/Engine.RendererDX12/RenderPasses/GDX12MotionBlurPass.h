#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

// Inputs: linear HDR, reverse-Z depth, unjittered prevNDC-currentNDC motion.
// Outputs: linear HDR at the input grid, complete NDC motion (including sky/particle coverage).
class GDX12MotionBlurPass final : public GDX12RenderPass
{
public:
    void RecordRenderGraph(GDX12RenderGraphBuilder& graph) override
    {
        graph.SetName("Motion blur");
        graph.Read(SharedResource::Scene);
        graph.ReadInputs(_inputs);
        graph.Read(SharedResource::Particles);
        graph.Write(_outputs[0], "Motion-blurred HDR");
        // Frame generation can retain motion beyond this command list.
        graph.Write(_outputs[1], "Complete motion", Lifetime::Persistent);
    }

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

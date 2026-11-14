#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

namespace Meta { class ScenePostUI; }

class GDX12DepthOfFieldPass final : public GDX12RenderPass
{
public:
    void RecordRenderGraph(GDX12RenderGraphBuilder& graph) override
    {
        graph.SetName("Depth of field");
        graph.Read(SharedResource::Scene);
        graph.ReadInputs(_inputs);
        graph.Write(_outputs[0], "DoF HDR");
    }

    GDX12DepthOfFieldPass();
    ~GDX12DepthOfFieldPass() override;
    // Input 0: linear RGBA16F HDR. Input 1: matching reverse-Z depth. Output 0: RGBA16F HDR.
    void Initialize() override;
    void Execute(GDX12CommandList* commands) override;
    void PrepareRecording(); // Owner-only allocation/retirement before explicit recording.
    void RecordCommands(GDX12CommandList* commands);
    void Resize() override;
    void ClearDenendencies() override;

private:
    friend class Meta::ScenePostUI;
    struct Context;
    void DestroyContext();
    std::unique_ptr<Context> _context;
    GDX12Texture _output;
    bool _wasEnabled = false;
    float _previousScale = 0.0f;
    float _previousBias = 0.0f;
};

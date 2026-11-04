#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

// Inputs: writable linear HDR and opaque reverse-Z depth. Does not enter probe captures.
class GDX12ReflectionPreviewPass final : public GDX12RenderPass
{
public:
    GDX12ReflectionPreviewPass();
    void RecordRenderGraph(GDX12RenderGraphBuilder& graph) override
    {
        graph.SetName("Reflection probe spheres");
        graph.Read(SharedResource::Scene);
        graph.Read(SharedResource::ReflectionProbes);
        graph.ReadWrite(_inputs[0]);
        graph.Read(_inputs[1]);
    }
    void Initialize() override;
    void Execute(GDX12CommandList* commands) override;

private:
    std::unique_ptr<GDX12RootSignature> _root;
    ComPtr<ID3D12PipelineState> _pipeline;
};

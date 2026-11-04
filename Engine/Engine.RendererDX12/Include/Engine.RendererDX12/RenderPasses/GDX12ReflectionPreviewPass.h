#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

// Forward unlit meshes after lighting; inputs: HDR, depth, motion, normals, F0/baked.
class GDX12ReflectionPreviewPass final : public GDX12RenderPass
{
public:
    GDX12ReflectionPreviewPass();
    void RecordRenderGraph(GDX12RenderGraphBuilder& graph) override
    {
        graph.SetName("Reflection probe meshes");
        graph.Read(SharedResource::Scene);
        graph.Read(SharedResource::ReflectionProbes);
        for (auto* input : _inputs) graph.ReadWrite(input);
    }
    void Initialize() override;
    void Execute(GDX12CommandList* commands) override;
private:
    std::unique_ptr<GDX12RootSignature> _root;
    ComPtr<ID3D12PipelineState> _pipeline;
};

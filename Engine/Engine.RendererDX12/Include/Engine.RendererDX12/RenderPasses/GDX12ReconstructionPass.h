#pragma once
#include "Engine.RendererDX12/GDX12Reconstruction.h"
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

class GDX12ReconstructionPass final : public GDX12RenderPass
{
public:
    void RecordRenderGraph(GDX12RenderGraphBuilder& graph) override
    {
        graph.SetName("Reconstruction");
        graph.Read(SharedResource::Scene);
        graph.ReadInputs(_inputs);
        graph.Write(_outputs[0], "Reconstructed or native HDR", Lifetime::Dynamic);
        graph.Fallback(_outputs[0], _inputs[2]);
    }

    GDX12ReconstructionPass();
    ~GDX12ReconstructionPass() override;
    void Setup(GDX12DeviceResources*, GDX12DeviceResources*, RenderPipelineCommonData*) override;
    void Initialize() override;
    void Execute(GDX12CommandList*) override;
    void Resize() override;
    void QueryRenderTargetResolution() override;
    void ClearDenendencies() override;
    bool ApplySettings();
    const GDX12ReconstructionStatus& GetStatus() const { return _status; }

private:
    struct StableOutputLink final : IRenderPassLink
    {
        IRenderPassLink* Target = nullptr;
        GDX12Texture* GetTexture() override { return Target ? Target->GetTexture() : nullptr; }
        bool IsInitialized() override { return Target && Target->IsInitialized(); }
    } _output;
    void Configure();
    void LinkOutput();
    std::unique_ptr<GDX12RenderPass> _provider;
    GDX12ReconstructionStatus _status;
    bool _needsInitialize = true;
};

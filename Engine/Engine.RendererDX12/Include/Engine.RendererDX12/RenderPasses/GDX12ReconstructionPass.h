#pragma once
#include "Engine.RendererDX12/GDX12Reconstruction.h"
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

namespace Meta { class ScenePostUI; }
class GDX12TAAPass;

class GDX12ReconstructionPass final : public GDX12RenderPass
{
public:

    GDX12ReconstructionPass();
    ~GDX12ReconstructionPass() override;
    void Setup(GDX12DeviceResources*, RenderPipelineCommonData*) override;
    void SetInputs(std::vector<IRenderPassLink*> inputs) override;
    void Initialize() override;
    void Execute(GDX12CommandList*) override;
    void Resize() override;
    void QueryRenderTargetResolution() override;
    void ClearDenendencies() override;
    bool ApplySettings();
    GDX12TAAPass* TAA() const;
    void PrepareRecording();
    void Submitted();
    void CancelRecording();
    const GDX12ReconstructionStatus& GetStatus() const { return _status; }

private:
    friend class Meta::ScenePostUI;
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

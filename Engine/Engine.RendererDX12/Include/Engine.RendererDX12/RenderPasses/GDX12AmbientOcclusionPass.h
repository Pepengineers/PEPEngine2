#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

// Inputs: reverse-Z camera depth, world-space normal/roughness. Output: AO visibility.
class GDX12AmbientOcclusionPass final : public GDX12RenderPass
{
public:
    GDX12AmbientOcclusionPass();
    ~GDX12AmbientOcclusionPass() override;
    void Initialize() override;
    void Execute(GDX12CommandList* commands) override;
    void Resize() override;
    void ClearDenendencies() override;
private:
    struct Data;
    std::unique_ptr<Data> _data;
};

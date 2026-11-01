#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

// Runs after transparent meshes; Composite runs after the existing WBOIT resolve.
class GDX12ParticlesPass final : public GDX12RenderPass
{
public:
    GDX12ParticlesPass();
    ~GDX12ParticlesPass() override;
    void Initialize() override;
    void Execute(GDX12CommandList* commands) override;
    void Composite(GDX12CommandList* commands, GDX12Texture* hdr);
    void Resize() override;
    void ClearDenendencies() override;

private:
    struct State;
    std::unique_ptr<State> _state;
};

#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

// Runs after transparent meshes; Composite runs after the existing WBOIT resolve.
class GDX12ParticlesPass final : public GDX12RenderPass
{
public:

    GDX12ParticlesPass();
    ~GDX12ParticlesPass() override;
    void Initialize() override;
    void BeginFrame();
    // Configure before BeginFrame; merged fluid depth is an SRV/UAV, not a DSV.
    void SetReadOnlyDepthEnabled(bool enabled) { _readOnlyDepth = enabled; }
    void SetMotionOutputEnabled(bool enabled) { _motionOutputEnabled = enabled; }
    std::vector<GDX12Resource*> NativeResources() const;
    std::vector<GDX12ResourceAccess> NativePreparationAccesses() const;
    std::vector<GDX12ResourceAccess> NativeRasterAccesses(bool includeMotion) const;
    GDX12Texture* MotionOutput() const;
    void PrepareRenderGroup(GDX12CommandList* commands, const std::vector<uint64_t>& emitterIds,
        bool clear, bool finalize);
    void DrawWeightedAndMotion(GDX12CommandList* commands);
    void Composite(GDX12CommandList* commands, GDX12Texture* hdr);
    void Resize() override;
    void ClearDenendencies() override;

private:
    void PrepareRenderData(GDX12CommandList* commands, const std::vector<uint64_t>* emitterIds,
        bool clear, bool finalize);
    bool _readOnlyDepth = false;
    bool _motionOutputEnabled = true;
    struct State;
    std::unique_ptr<State> _state;
};

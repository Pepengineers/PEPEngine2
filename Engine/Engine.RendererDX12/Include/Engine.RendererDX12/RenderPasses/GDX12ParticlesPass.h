#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include <array>

struct GDX12ParticleRenderFrame;

// Opaque particles write the G-buffer; transparent particles use WBOIT and HDR composition.
class GDX12ParticlesPass final : public GDX12RenderPass
{
public:

    GDX12ParticlesPass();
    ~GDX12ParticlesPass() override;
    void Initialize() override;
    void BeginFrame(GDX12Texture* depth, const GDX12ParticleRenderFrame& frame);
    enum class OpaqueStage : UINT { Combined, DepthNormals, GBuffer };
    bool InitializeOpaque(const std::array<GDX12Texture*, 5>& gbuffer, bool depthNormals);
    // Configure before BeginFrame; merged fluid depth is an SRV/UAV, not a DSV.
    void SetReadOnlyDepthEnabled(bool enabled) { _readOnlyDepth = enabled; }
    void SetMotionOutputEnabled(bool enabled) { _motionOutputEnabled = enabled; }
    std::vector<GDX12Resource*> NativeResources() const;
    std::vector<GDX12ResourceAccess> NativePreparationAccesses(const GDX12ParticleRenderFrame& frame, bool opaque) const;
    std::vector<GDX12ResourceAccess> NativeRasterAccesses(bool includeMotion) const;
    GDX12Texture* MotionOutput() const;
    void PrepareRenderData(GDX12CommandList* commands, const GDX12ParticleRenderFrame& frame, bool opaque);
    void DrawOpaque(GDX12CommandList* commands, GDX12Texture* depth, D3D12_CPU_DESCRIPTOR_HANDLE dsv,
        const D3D12_CPU_DESCRIPTOR_HANDLE* targets, UINT targetCount, OpaqueStage);
    void DrawWeightedAndMotion(GDX12CommandList* commands);
    void Composite(GDX12CommandList* commands, GDX12Texture* hdr);
    void Resize() override;
    void ClearDenendencies() override;

private:
    bool _readOnlyDepth = false;
    bool _motionOutputEnabled = true;
    struct State;
    std::unique_ptr<State> _state;
};

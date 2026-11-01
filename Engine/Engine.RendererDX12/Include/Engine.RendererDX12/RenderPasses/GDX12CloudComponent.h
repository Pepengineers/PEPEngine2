#pragma once
#include <cstdint>
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include "Engine.RendererDX12/GDX12RootSignature.h"
#include "Engine.RendererDX12/GDX12Texture.h"

// Render-thread API. Zero octaves inherits quality; author fields are never rewritten.
struct GDX12CloudParameters
{
    bool Enabled = true;
    int Implementation = 0; // 0 fBM, 1 ridged simplex.
    UINT Octaves = 0;
    float Scale = 4.0f;
    float Coverage = 0.5f;
    float Softness = 0.15f;
    Vector2 Speed = Vector2(0.02f, 0.01f); // Simplex lattice units / simulation second.
    Vector3 HorizonColor = Vector3(0.35f, 0.5f, 0.8f);
    Vector3 ZenithColor = Vector3(0.05f, 0.15f, 0.4f);
    Vector3 CloudColor = Vector3(2.0f); // Linear HDR radiance.
};

// Explicit source, never installed into the scene automatically. One instance per source.
class GDX12CloudComponent final : public GDX12RenderPass
{
public:
    GDX12CloudComponent();
    void Setup(GDX12DeviceResources* resources, GDX12DeviceResources* other, RenderPipelineCommonData* common) override;
    void SetParameters(const GDX12CloudParameters& parameters);
    const GDX12CloudParameters& GetParameters() const { return _parameters; }
    void Advance(double simulationDeltaSeconds); // Pass zero while paused; once per simulation tick.
    void Reset();
    bool IsActive() const;
    UINT GetQuality() const;
    uint64_t GetSourceRevision() const { return _sourceRevision; }
    uint64_t GetLightingRevision() const { return _lightingRevision; }
    void Execute(GDX12CommandList* commands) override;
    void ClearDenendencies() override; // Caller retires submitted work first, as for other passes.

private:
    void CreateResources();
    GDX12CloudParameters _parameters;
    std::unique_ptr<GDX12Texture> _texture;
    std::unique_ptr<GDX12RootSignature> _root;
    ComPtr<ID3D12PipelineState> _pipeline;
    double _phaseX = 0;
    double _phaseY = 0;
    uint64_t _sourceRevision = 1;
    uint64_t _lightingRevision = 1;
    uint64_t _renderedRevision = 0;
    int _quality = -1;
    bool _enabled = false;
};

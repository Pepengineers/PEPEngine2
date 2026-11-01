#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include "Engine.RendererDX12/GDX12Fluid.h"
#include <map>

class GDX12FluidSurfacePass final : public GDX12RenderPass
{
public:
    void RecordRenderGraph(GDX12RenderGraphBuilder& graph) override
    {
        graph.SetName("Fluid surface");
        graph.Read(SharedResource::Scene);
        graph.ReadInputs(_inputs);
        graph.Write(_outputs[0], "Fluid or opaque depth", Lifetime::Dynamic);
        graph.Write(_outputs[1], "Fluid or opaque motion", Lifetime::Dynamic);
        graph.Fallback(_outputs[0], _inputs[0]);
        graph.Fallback(_outputs[1], _inputs[1]);
        graph.Write(SharedResource::FluidSurface, "Fluid simulation and surface");
        graph.HasSideEffects();
    }

    GDX12FluidSurfacePass();
    ~GDX12FluidSurfacePass() override;
    void Initialize() override;
    void Execute(GDX12CommandList* commands) override;
    void Resize() override;
    void ClearDenendencies() override;
    GDX12Texture* Resolve(GDX12CommandList* commands, GDX12Texture* background, GDX12Texture* environment);
private:
    struct Link final : IRenderPassLink
    {
        GDX12Texture* Target = nullptr;
        GDX12Texture* GetTexture() override { return Target; }
        bool IsInitialized() override { return Target && Target->IsInitialized(); }
    };
    struct Parameters;
    struct State;
    void CreatePipelines();
    void CreateTargets();
    void ReleaseTargets();
    void PrepareStates();
    void Simulate(GDX12CommandList* commands, State& state, const GDX12FluidSettings& settings, float elapsed);
    void DrawSurface(GDX12CommandList* commands, State& state, UINT owner, const GDX12FluidSettings& settings);
    void SurfaceBindings(GDX12CommandList* commands, State& state, UINT owner, UINT mode = 0);
    Link _depthLink, _motionLink;
    std::map<uint64_t, std::unique_ptr<State>> _states;
    std::unique_ptr<GDX12RootSignature> _solverRoot, _surfaceRoot;
    ComPtr<ID3D12PipelineState> _save, _hash, _sort, _offsets, _densities, _integrate;
    ComPtr<ID3D12PipelineState> _sphere, _thicknessDraw, _initialize, _filter, _merge, _resolve;
    std::unique_ptr<GDX12Texture> _raw, _eyeDepth, _scratchDepth, _thickness;
    std::unique_ptr<GDX12Texture> _filtered[2];
    std::unique_ptr<GDX12Texture> _depth, _motion, _geometry, _owner, _color;
    GDX12FluidControls _controls = { false, 0 };
    bool _rendering = false;
};

class GDX12FluidResolvePass final : public GDX12RenderPass
{
public:
    void RecordRenderGraph(GDX12RenderGraphBuilder& graph) override
    {
        graph.SetName("Fluid resolve");
        graph.Read(SharedResource::Scene);
        graph.ReadInputs(_inputs);
        graph.Read(SharedResource::FluidSurface);
        graph.Write(_outputs[0], "Water HDR", Lifetime::Dynamic);
        graph.Fallback(_outputs[0], _inputs[0]);
    }

    explicit GDX12FluidResolvePass(GDX12FluidSurfacePass* surface);
    void Initialize() override;
    void Execute(GDX12CommandList* commands) override;
    void ClearDenendencies() override;
private:
    struct Link final : IRenderPassLink
    {
        GDX12Texture* Target = nullptr;
        GDX12Texture* GetTexture() override { return Target; }
        bool IsInitialized() override { return Target && Target->IsInitialized(); }
    } _color;
    GDX12FluidSurfacePass* _surface;
};

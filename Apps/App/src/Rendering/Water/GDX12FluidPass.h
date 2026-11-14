#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include "Rendering/Water/GDX12FluidSolver.h"
#include <map>

class GDX12FluidSurfacePass final : public GDX12RenderPass
{
public:
    void RecordRenderGraph(GDX12RenderGraphBuilder& graph) override
    {
        graph.SetName("Fluid surface");
        graph.Read(SharedResource::Scene);
        graph.ReadInputs(_inputs, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        graph.Write(_outputs[0], "Fluid or opaque depth", Lifetime::Dynamic);
        graph.Write(_outputs[1], "Fluid or opaque motion", Lifetime::Dynamic);
        graph.Fallback(_outputs[0], _inputs[0]);
        graph.Fallback(_outputs[1], _inputs[1]);
        graph.Write(_outputs[2], "Fluid geometry", Lifetime::Dynamic);
        graph.Write(_outputs[3], "Fluid owners", Lifetime::Dynamic);
        graph.HasSideEffects();
    }

    GDX12FluidSurfacePass();
    ~GDX12FluidSurfacePass() override;
    void Initialize() override;
    void Execute(GDX12CommandList* commands) override;
    void SetInstances(std::vector<GDX12FluidInstance> instances);
    void PrepareFrame();
    void SetLocalExecution(bool enabled) { _localOnly = enabled; }
    // Opt-in recording-only display: caller owns seed/history and GPU lifetime.
    using ExternalDisplay = std::map<uint64_t, GDX12UploadBuffer<GDX12FluidSolver::Particle>*>;
    void SetExternalDisplay(ExternalDisplay display) { _externalOnly = true; _externalDisplay = std::move(display); }
    std::vector<GDX12Resource*> NativeResources() const;
    GDX12Texture* NativeColor() const { return _rendering ? _color.get() : nullptr; }
    void ExecuteSimulation(GDX12CommandList* commands);
    std::vector<GDX12RenderGraph::BufferAccess> GetSimulationBufferAccesses() const;
    std::vector<GDX12RenderGraph::BufferAccess> GetBufferAccesses() const override;
    std::vector<GDX12RenderGraph::BufferAccess> GetResolveBufferAccesses() const;
    IRenderPassLink* GetOpaqueDepth() const { return _inputs[0]; }
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
    struct State;
    struct ParticleView
    {
        GDX12Resource& Resource;
        D3D12_GPU_DESCRIPTOR_HANDLE SRV;
        UINT Count;
    };
    void CreatePipelines();
    void CreateTargets();
    void ReleaseTargets();
    void PrepareStates();
    void DrawSurface(GDX12CommandList* commands, const ParticleView& particles,
        D3D12_GPU_VIRTUAL_ADDRESS constants, UINT owner,
        const GDX12FluidSettings& settings);
    void SurfaceBindings(GDX12CommandList* commands, D3D12_GPU_VIRTUAL_ADDRESS constants, UINT owner, UINT mode = 0);
    Link _depthLink, _motionLink, _geometryLink, _ownerLink;
    std::vector<GDX12FluidInstance> _instances;
    std::map<uint64_t, std::unique_ptr<State>> _states;
    GDX12FluidSolver _solver;
    std::unique_ptr<GDX12RootSignature> _surfaceRoot;
    ComPtr<ID3D12PipelineState> _sphere, _thicknessDraw, _initialize, _filter, _merge, _resolve;
    std::unique_ptr<GDX12Texture> _raw, _eyeDepth, _scratchDepth, _thickness;
    std::unique_ptr<GDX12Texture> _filtered[2];
    std::unique_ptr<GDX12Texture> _depth, _motion, _geometry, _owner, _color;
    GDX12FluidControls _controls = { false, 0 };
    bool _localOnly = false;
    bool _externalOnly = false;
    ExternalDisplay _externalDisplay;
    bool _rendering = false;
    bool _simulating = false;
    bool _prepared = false;
};

class GDX12FluidSimulationPass final : public GDX12RenderPass
{
public:
    explicit GDX12FluidSimulationPass(GDX12FluidSurfacePass* surface) : _surface(surface) {}
    void RecordRenderGraph(GDX12RenderGraphBuilder& graph) override
    {
        graph.SetName("Fluid simulation");
        graph.AsyncCompute();
        graph.Read(SharedResource::Scene);
        graph.HasSideEffects();
    }
    bool IsAsyncComputeEnabled() const override { return GetFluidControls().AsyncCompute; }
    void Execute(GDX12CommandList* commands) override { _surface->ExecuteSimulation(commands); }
    std::vector<GDX12RenderGraph::BufferAccess> GetBufferAccesses() const override
    { return _surface->GetSimulationBufferAccesses(); }
private:
    GDX12FluidSurfacePass* _surface;
};

class GDX12FluidResolvePass final : public GDX12RenderPass
{
public:
    void RecordRenderGraph(GDX12RenderGraphBuilder& graph) override
    {
        graph.SetName("Fluid resolve");
        graph.Read(SharedResource::Scene);
        graph.ReadInputs(_inputs);
        graph.Read(_surface->GetOpaqueDepth());
        graph.Read(_surface->GetOutput(0));
        graph.Read(_surface->GetOutput(2));
        graph.Read(_surface->GetOutput(3));
        graph.Read(SharedResource::ReflectionProbes);
        graph.Write(_outputs[0], "Water HDR", Lifetime::Dynamic);
        graph.Fallback(_outputs[0], _inputs[0]);
    }

    explicit GDX12FluidResolvePass(GDX12FluidSurfacePass* surface);
    void Initialize() override;
    void PrepareFrame();
    void Execute(GDX12CommandList* commands) override;
    std::vector<GDX12RenderGraph::BufferAccess> GetBufferAccesses() const override
    { return _surface->GetResolveBufferAccesses(); }
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

#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include "Rendering/Water/GDX12FluidSolver.h"
#include <map>
#include <functional>

class GDX12FluidSurfacePass final : public GDX12RenderPass
{
public:
    GDX12FluidSurfacePass();
    ~GDX12FluidSurfacePass() override;
    void Initialize() override;
    void Execute(GDX12CommandList* commands) override;
    void SetInstances(std::vector<GDX12FluidInstance> instances);
    void PrepareFrame();
    void SetStaticIBL(bool enabled) { _staticIBL = enabled; }
    void SetParticleRendering(bool enabled) { _particleRendering = enabled; }
    void DrawParticles(GDX12CommandList*, GDX12Texture* hdr, D3D12_CPU_DESCRIPTOR_HANDLE depth);
    void SetResourceRetirement(std::function<void()> retire) { _retire = std::move(retire); }
    bool IsRendering() const { return _rendering; }
    bool NeedsSimulation() const { return _rendering || _simulating; }
    std::vector<GDX12ResourceAccess> NativeSurfaceAccesses() const;
    void ExecuteSimulation(GDX12CommandList* commands);
    std::vector<GDX12ResourceAccess> GetSimulationBufferAccesses() const;
    void Resize() override;
    void ClearDenendencies() override;
    GDX12Texture* Resolve(GDX12CommandList* commands, GDX12Texture* background, GDX12Texture* environment,
        D3D12_GPU_DESCRIPTOR_HANDLE probeRadiance = {});
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
    void RetireResources();
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
    ComPtr<ID3D12PipelineState> _particleDraw;
    std::unique_ptr<GDX12Texture> _raw, _eyeDepth, _scratchDepth, _thickness;
    std::unique_ptr<GDX12Texture> _filtered[2];
    std::unique_ptr<GDX12Texture> _depth, _motion, _geometry, _owner, _color;
    GDX12FluidControls _controls = { false, 0 };
    bool _staticIBL = false;
    bool _particleRendering = false;
    std::function<void()> _retire;
    bool _rendering = false;
    bool _simulating = false;
    bool _prepared = false;
};

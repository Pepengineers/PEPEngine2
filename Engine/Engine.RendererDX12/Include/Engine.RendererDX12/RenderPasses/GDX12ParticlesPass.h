#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

class GDX12ParticleSimulationPass final : public GDX12RenderPass
{
public:
    GDX12ParticleSimulationPass()
    {
        _flags = RENDER_PASS_FLAG_USE_CAMERAS;
        _numInputs = 1;
    }
    void RecordRenderGraph(GDX12RenderGraphBuilder& graph) override
    {
        graph.SetName("Particle simulation");
        graph.Read(SharedResource::Scene);
        graph.Read(_inputs[0]);
        graph.Write(SharedResource::ParticleSimulation, "Particle simulation pools");
        graph.HasSideEffects();
    }
    void Execute(GDX12CommandList* commands) override
    {
        if (_resources->ParticleSimulation)
            _resources->ParticleSimulation->Execute(commands, _resources, *_commonData, _inputs[0]->GetTexture());
    }
};

// Runs after transparent meshes; Composite runs after the existing WBOIT resolve.
class GDX12ParticlesPass final : public GDX12RenderPass
{
public:
    void RecordRenderGraph(GDX12RenderGraphBuilder& graph) override
    {
        graph.SetName("Particles");
        graph.Read(SharedResource::Scene);
        graph.Read(SharedResource::ParticleSimulation);
        graph.Read(_inputs[0]);
        graph.ReadWrite(_inputs[1]);
        graph.ReadWrite(_inputs[2]);
        graph.Write(SharedResource::Particles, "Particles and feature motion");
        graph.HasSideEffects();
    }

    GDX12ParticlesPass();
    ~GDX12ParticlesPass() override;
    void Initialize() override;
    void BeginFrame();
    void Execute(GDX12CommandList* commands) override;
    void Composite(GDX12CommandList* commands, GDX12Texture* hdr);
    void Resize() override;
    void ClearDenendencies() override;

private:
    struct State;
    std::unique_ptr<State> _state;
};

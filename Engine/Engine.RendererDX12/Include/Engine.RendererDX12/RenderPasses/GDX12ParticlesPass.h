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
        graph.AsyncCompute();
        graph.Read(SharedResource::Scene);
        graph.Read(_inputs[0], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        graph.Write(SharedResource::ParticleSimulation, "Particle simulation pools");
        graph.HasSideEffects();
    }
    bool IsAsyncComputeEnabled() const override { return GetParticleSettings().AsyncCompute; }
    void Execute(GDX12CommandList* commands) override
    {
        if (_resources->ParticleSimulation)
            _resources->ParticleSimulation->Execute(commands, _resources, *_commonData, _inputs[0]->GetTexture());
    }
    std::vector<GDX12RenderGraph::BufferAccess> GetBufferAccesses() const override
    {
        std::vector<GDX12RenderGraph::BufferAccess> result;
        if (_resources->ParticleSimulation)
            for (auto* resource : _resources->ParticleSimulation->GetPoolResources())
                result.push_back({ resource, GDX12RenderGraph::Mode::ReadWrite });
        if (_resources->ParticleSimulation)
            if (auto* interaction = _resources->ParticleSimulation->GetInteractionResource())
                result.push_back({ interaction, GDX12RenderGraph::Mode::Read });
        const auto& frame = _resources->FrameConstants[_resources->CurrFrameConstantsIndex];
        if (frame->CameraCB && frame->CameraCB->GetResource().D3DResource)
            result.push_back({ &frame->CameraCB->GetResource(), GDX12RenderGraph::Mode::Read,
                D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER });
        return result;
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
    std::vector<GDX12RenderGraph::BufferAccess> GetBufferAccesses() const override
    {
        std::vector<GDX12RenderGraph::BufferAccess> result;
        if (_resources->ParticleSimulation)
            for (auto* resource : _resources->ParticleSimulation->GetPoolResources())
                result.push_back({ resource, GDX12RenderGraph::Mode::Read });
        const auto& frame = _resources->FrameConstants[_resources->CurrFrameConstantsIndex];
        if (frame->CameraCB && frame->CameraCB->GetResource().D3DResource)
            result.push_back({ &frame->CameraCB->GetResource(), GDX12RenderGraph::Mode::Read,
                D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER });
        return result;
    }
    void Execute(GDX12CommandList* commands) override;
    void Composite(GDX12CommandList* commands, GDX12Texture* hdr);
    void Resize() override;
    void ClearDenendencies() override;

private:
    struct State;
    std::unique_ptr<State> _state;
};

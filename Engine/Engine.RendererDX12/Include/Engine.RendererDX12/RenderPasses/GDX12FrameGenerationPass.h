#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include "Engine.RendererDX12/GDX12SwapChain.h"
#include "Engine.RendererDX12/GDX12FrameGeneration.h"

class GDX12FrameGenerationPass final : public GDX12RenderPass
{
public:
    explicit GDX12FrameGenerationPass(GDX12SwapChain* swapchain) : _swapchain(swapchain) { _numInputs = 3; }

    void RecordRenderGraph(GDX12RenderGraphBuilder& graph) override
    {
        graph.SetName("Frame generation inputs");
        // Reading the display also orders preparation after tone mapping.
        graph.ReadInputs(_inputs);
        graph.Export(_inputs[0]);
        graph.Export(_inputs[1]);
        graph.HasSideEffects();
    }

    void Execute(GDX12CommandList* commands) override
    {
        _swapchain->GetFrameGeneration().Prepare(commands,
            _inputs[0]->GetTexture(), _inputs[1]->GetTexture(), *_commonData);
    }
private:
    GDX12SwapChain* _swapchain;
};

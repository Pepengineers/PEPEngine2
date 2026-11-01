#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

class GDX12OutputToScreenPass final : public GDX12RenderPass
{
public:
    void RecordRenderGraph(GDX12RenderGraphBuilder& graph) override
    {
        graph.SetName("Tone map");
        graph.Read(_inputs[0], D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        graph.Write(_inputs[1], "Swapchain", Lifetime::Dynamic, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }

    GDX12OutputToScreenPass() { _numInputs = 2; }

    // Linear HDR input, display UNORM back buffer. Encode once, after upscaling.
    void Initialize() override
    {
        GDX12RootSignatureDesc root;
        root.NumSingleSRVSlots = 1;
        root.StaticSamplers = GetStaticSamplers();
        _root = std::make_unique<GDX12RootSignature>(_resources->Device, root);
        auto& compiler = GDX12ShaderCompiler::GetInstance();
        auto vs = compiler.CompileShader(_resources->Device, SHADERS_FOLDER "FullScreenVS.hlsl", nullptr, "VS", "vs");
        auto ps = compiler.CompileShader(_resources->Device, SHADERS_FOLDER "ToneMap.hlsl", nullptr, "PS", "ps");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = _root->GetRootSignature().Get();
        desc.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
        desc.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
        desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        desc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        desc.DepthStencilState.DepthEnable = false;
        desc.DepthStencilState.StencilEnable = false;
        desc.SampleMask = UINT_MAX;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = _inputs[1]->GetTexture()->GetFormat();
        desc.SampleDesc.Count = 1;
        ThrowIfFailed(_resources->Device->GetDevice()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&_pipeline)));
    }

    void Execute(GDX12CommandList* commands) override
    {
        auto* input = _inputs[0]->GetTexture();
        auto* output = _inputs[1]->GetTexture();
        GDX12_SCOPED_EVENT(commands, "HDR tone map to display", Colors::Aqua);
        // Full-screen overwrite still requires explicit initialization of placed render targets.
        commands->GetCommandList()->DiscardResource(output->GetResource()->D3DResource.Get(), nullptr);
        commands->SetGraphicsRootSignature(_root.get());
        commands->SetPipelineState(_pipeline);
        commands->SetDescriptorHeaps({ _resources->SRV_UAV_Heap.get() });
        commands->SetGraphicsSRV(0, input->GetSRV()->GPUHandle);
        commands->SetViewport(output->GetViewport());
        commands->SetScissorRect(output->GetScissorRect());
        commands->SetRenderTargets({ output }, nullptr);
        commands->SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commands->DrawInstanced(3, 1, 0, 0);
        commands->ResourceBarrier({ output->GetResource()->GetPresentBarrier() });
    }

    void ClearDenendencies() override
    {
        GDX12RenderPass::ClearDenendencies();
        _root.reset();
        _pipeline.Reset();
    }
private:
    std::unique_ptr<GDX12RootSignature> _root;
    ComPtr<ID3D12PipelineState> _pipeline;
};

#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

class GDX12OutputToScreenPass final : public GDX12RenderPass
{
public:
    GDX12OutputToScreenPass() { _numInputs = 2; }

    // Input 0: linear HDR; input 1: UNORM swap-chain back buffer.
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
        desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        desc.RasterizerState.FrontCounterClockwise = TRUE;
        desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        desc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        desc.DepthStencilState.DepthEnable = false;
        desc.DepthStencilState.StencilEnable = false;
        desc.SampleMask = UINT_MAX;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = _inputs[1]->GetTexture()->GetFormat();
        desc.SampleDesc.Count = 1;
        desc.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
        desc.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
        ThrowIfFailed(_resources->Device->GetDevice()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&_pipeline)));
    }

    void Execute(GDX12CommandList* commands) override
    {
        auto* input = _inputs[0]->GetTexture();
        auto* target = _inputs[1]->GetTexture();
        commands->BeginPixEvent("HDR tone mapping", Colors::Aqua);
        commands->SetGraphicsRootSignature(_root.get());
        commands->SetPipelineState(_pipeline.Get());
        commands->SetDescriptorHeaps({ _resources->SRV_UAV_Heap.get() });
        commands->SetViewport(target->GetViewport());
        commands->SetScissorRect(target->GetScissorRect());
        commands->ResourceBarrier({ input->GetResource()->GetSRVBarrier(),
            target->GetResource()->GetRenderTargetBarrier() });
        commands->SetRenderTargets({ target }, nullptr);
        commands->SetGraphicsSRV(0, input->GetSRV()->GPUHandle);
        commands->GetCommandList()->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commands->GetCommandList()->DrawInstanced(3, 1, 0, 0);
        commands->ResourceBarrier({ target->GetResource()->GetPresentBarrier() });
        commands->EndPixEvent();
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

#pragma once

#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

class GDX12OutputToScreenPass : public GDX12RenderPass
{
public:
    GDX12OutputToScreenPass() : IN_Texture(nullptr), IN_BackBuffer(nullptr)
    {
        _flags = RENDER_PASS_FLAG_NONE;
        _numInputs = 2;
        _numOutputs = 0;
    }

    // Input 0 - LinearHDRTexture
    // Input 1 - BackBuffer
    void Initialize() override
    {
        IN_Texture = _inputs[0];
        IN_BackBuffer = _inputs[1];
        auto& compiler = GDX12ShaderCompiler::GetInstance();
        auto vs = compiler.CompileShader(_resources->Device, SHADERS_FOLDER "FullScreenVS.hlsl", nullptr, "VS", "vs");
        auto ps = compiler.CompileShader(_resources->Device, SHADERS_FOLDER "ToneMapping.hlsl", nullptr, "PS", "ps");
        GDX12RootSignatureDesc root;
        root.NumSingleSRVSlots = 1;
        root.StaticSamplers = GetStaticSamplers();
        _rootSignature = std::make_unique<GDX12RootSignature>(_resources->Device, root);
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = _rootSignature->GetRootSignature().Get();
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
        desc.RTVFormats[0] = IN_BackBuffer->GetTexture()->GetFormat();
        desc.SampleDesc.Count = 1;
        ThrowIfFailed(_resources->Device->GetDevice()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&_pipelineState)));
    }

    void Execute(GDX12CommandList* cmdList) override
    {
        GDX12Texture* inputTexture = IN_Texture->GetTexture();
        GDX12Texture* backBuffer = IN_BackBuffer->GetTexture();

        cmdList->BeginPixEvent("Final HDR Tone Mapping", Colors::Aqua);
        cmdList->ResourceBarrier({ inputTexture->GetResource()->GetSRVBarrier(),
            backBuffer->GetResource()->GetRenderTargetBarrier() });
        cmdList->SetGraphicsRootSignature(_rootSignature.get());
        cmdList->SetPipelineState(_pipelineState);
        cmdList->SetDescriptorHeaps({ _resources->SRV_UAV_Heap.get() });
        cmdList->SetViewport(backBuffer->GetViewport());
        cmdList->SetScissorRect(backBuffer->GetScissorRect());
        cmdList->SetRenderTargets({ backBuffer }, nullptr);
        cmdList->SetGraphicsSRV(0, inputTexture->GetSRV()->GPUHandle);
        cmdList->SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmdList->DrawInstanced(3, 1, 0, 0);
        cmdList->ResourceBarrier({ backBuffer->GetResource()->GetPresentBarrier() });
        cmdList->EndPixEvent();
    }

    void ClearDenendencies() override
    {
        GDX12RenderPass::ClearDenendencies();

        IN_Texture = nullptr;
        IN_BackBuffer = nullptr;
    }

private:
    std::unique_ptr<GDX12RootSignature> _rootSignature;
    ComPtr<ID3D12PipelineState> _pipelineState;
    IRenderPassLink* IN_Texture;
    IRenderPassLink* IN_BackBuffer;
};

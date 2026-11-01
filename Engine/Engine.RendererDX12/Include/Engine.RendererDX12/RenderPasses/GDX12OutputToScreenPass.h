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

	// Input 0 - InputTexture
	// Input 1 - BackBuffer
	void Initialize() override
	{
		IN_Texture = _inputs[0];
		IN_BackBuffer = _inputs[1];
        GDX12RootSignatureDesc root;
        root.NumSingleSRVSlots = 1;
        root.StaticSamplers = GetStaticSamplers();
        _root = std::make_unique<GDX12RootSignature>(_resources->Device, root);
        auto& compiler = GDX12ShaderCompiler::GetInstance();
        auto vs = compiler.CompileShader(_resources->Device, SHADERS_FOLDER "FullScreenVS.hlsl", nullptr, "VS", "vs");
        auto ps = compiler.CompileShader(_resources->Device, SHADERS_FOLDER "ToneMap.hlsl", nullptr, "PS", "ps");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline = {};
        pipeline.pRootSignature = _root->GetRootSignature().Get();
        pipeline.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
        pipeline.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
        pipeline.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        pipeline.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pipeline.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        pipeline.DepthStencilState.DepthEnable = FALSE;
        pipeline.DepthStencilState.StencilEnable = FALSE;
        pipeline.SampleMask = UINT_MAX;
        pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pipeline.NumRenderTargets = 1;
        pipeline.RTVFormats[0] = IN_BackBuffer->GetTexture()->GetFormat();
        pipeline.SampleDesc.Count = 1;
        ThrowIfFailed(_resources->Device->GetDevice()->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&_pipeline)));
	}

	void Execute(GDX12CommandList* cmdList) override
	{
		GDX12Texture* inputTexture = IN_Texture->GetTexture();
		GDX12Texture* backBuffer = IN_BackBuffer->GetTexture();

        cmdList->BeginPixEvent("Tone map to screen", Colors::Aqua);
        cmdList->ResourceBarrier({ inputTexture->GetResource()->GetSRVBarrier(), backBuffer->GetResource()->GetRenderTargetBarrier() });
        cmdList->SetGraphicsRootSignature(_root.get());
        cmdList->SetPipelineState(_pipeline);
        cmdList->SetDescriptorHeaps({ _resources->SRV_UAV_Heap.get() });
        cmdList->SetGraphicsSRV(0, inputTexture->GetSRV()->GPUHandle);
        cmdList->SetViewport(backBuffer->GetViewport());
        cmdList->SetScissorRect(backBuffer->GetScissorRect());
        cmdList->SetRenderTargets({ backBuffer }, nullptr);
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
        _root.reset();
        _pipeline.Reset();
	}

private:
    std::unique_ptr<GDX12RootSignature> _root;
    ComPtr<ID3D12PipelineState> _pipeline;
	IRenderPassLink* IN_Texture;
	IRenderPassLink* IN_BackBuffer;
};
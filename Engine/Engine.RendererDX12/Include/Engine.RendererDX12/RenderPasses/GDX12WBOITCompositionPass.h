#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

class GDX12WBOITCompositionPass : public GDX12RenderPass
{
public:
	GDX12WBOITCompositionPass()  { _flags = RENDER_PASS_FLAG_NONE; }

	void Initialize(GDX12DeviceResources* resources, DXGI_FORMAT OUT_Format)
	{
		_resources = resources;

		// Shaders
		auto& shaderCompiler = GDX12ShaderCompiler::GetInstance();
		_compositionVS = shaderCompiler.CompileShader(resources->Device, SHADERS_FOLDER "FullScreenVS.hlsl", nullptr, "VS", "vs");
		_compositionPS = shaderCompiler.CompileShader(resources->Device, SHADERS_FOLDER "CompositionPass.hlsl", nullptr, "PS", "ps");

		// Root Signatures
		GDX12RootSignatureDesc RSDesc1;
		RSDesc1.NumSingleSRVSlots = 3;
		RSDesc1.StaticSamplers = GetStaticSamplers();
		_compositionRS = std::make_unique<GDX12RootSignature>(resources->Device, RSDesc1);

		// Pipeline State Objects
		D3D12_GRAPHICS_PIPELINE_STATE_DESC PSODesc1 = {};
		PSODesc1.InputLayout = { nullptr, 0 };
		PSODesc1.pRootSignature = _compositionRS->GetRootSignature().Get();
		PSODesc1.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		PSODesc1.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		PSODesc1.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
		//reversed-Z
		PSODesc1.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
		PSODesc1.RasterizerState.FrontCounterClockwise = TRUE;
		PSODesc1.SampleMask = UINT_MAX;
		PSODesc1.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		PSODesc1.NumRenderTargets = 1;
		PSODesc1.RTVFormats[0] = OUT_Format;
		PSODesc1.SampleDesc.Count = 1;
		PSODesc1.SampleDesc.Quality = 0;
		PSODesc1.DepthStencilState.DepthEnable = false;
		PSODesc1.DepthStencilState.StencilEnable = false;
		PSODesc1.VS = { reinterpret_cast<BYTE*>(_compositionVS->GetBufferPointer()), _compositionVS->GetBufferSize() };
		PSODesc1.PS = { reinterpret_cast<BYTE*>(_compositionPS->GetBufferPointer()), _compositionPS->GetBufferSize() };
		ThrowIfFailed(resources->Device->GetDevice()->CreateGraphicsPipelineState(&PSODesc1, IID_PPV_ARGS(&_compositionPSO)));
	}

	void Execute(GDX12CommandList* cmdList, GDX12Texture* IN_OpaqueScene, GDX12Texture* IN_TransparencyAccum,
		GDX12Texture* IN_Revealage, GDX12Texture* IN_OUT_Result)
	{
		cmdList->BeginPixEvent("Composition Render Pass", Colors::Bisque);
		cmdList->SetViewport(IN_OUT_Result->GetViewport());
		cmdList->SetScissorRect(IN_OUT_Result->GetScissorRect());
		cmdList->SetGraphicsRootSignature(_compositionRS.get());
		cmdList->SetPipelineState(_compositionPSO.Get());
		cmdList->SetRenderTargets({ IN_OUT_Result }, nullptr);
		cmdList->SetDescriptorHeaps({ _resources->SRV_UAV_Heap.get() });
		cmdList->SetGraphicsSRV(0, IN_OpaqueScene->GetSRV()->GPUHandle);
		cmdList->SetGraphicsSRV(1, IN_TransparencyAccum->GetSRV()->GPUHandle);
		cmdList->SetGraphicsSRV(2, IN_Revealage->GetSRV()->GPUHandle);
		cmdList->GetCommandList()->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		cmdList->GetCommandList()->DrawInstanced(3, 1, 0, 0);
		cmdList->EndPixEvent();
	}
private:
	ComPtr<ID3DBlob> _compositionVS;
	ComPtr<ID3DBlob> _compositionPS;
	std::unique_ptr<GDX12RootSignature> _compositionRS;
	ComPtr<ID3D12PipelineState> _compositionPSO;
};
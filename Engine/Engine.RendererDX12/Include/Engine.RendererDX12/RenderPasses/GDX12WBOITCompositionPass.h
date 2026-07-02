#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

class GDX12WBOITCompositionPass : public GDX12RenderPass
{
public:
	GDX12WBOITCompositionPass() : IN_OpaqueScene(nullptr), IN_TransparencyAccum(nullptr),
		IN_Revealage(nullptr), IN_OUT_Result(nullptr)
	{ _flags = RENDER_PASS_FLAG_NONE; }

	void LinkDependancies(IRenderPassLink* IN_OpaqueScene, IRenderPassLink* IN_TransparencyAccum,
		IRenderPassLink* IN_Revealage, IRenderPassLink* IN_OUT_Result)
	{
		this->IN_OpaqueScene = IN_OpaqueScene;
		this->IN_TransparencyAccum = IN_TransparencyAccum;
		this->IN_Revealage = IN_Revealage;
		this->IN_OUT_Result = IN_OUT_Result;

		PostLinkInitialize();
	}

	void Initialize(GDX12DeviceResources* resources, RenderPipelineCommonData* commonData) override
	{
		GDX12RenderPass::Initialize(resources, commonData);

		// Shaders
		auto& shaderCompiler = GDX12ShaderCompiler::GetInstance();
		_compositionVS = shaderCompiler.CompileShader(resources->Device, SHADERS_FOLDER "FullScreenVS.hlsl", nullptr, "VS", "vs");
		_compositionPS = shaderCompiler.CompileShader(resources->Device, SHADERS_FOLDER "CompositionPass.hlsl", nullptr, "PS", "ps");

		// Root Signatures
		GDX12RootSignatureDesc RSDesc1;
		RSDesc1.NumSingleSRVSlots = 3;
		RSDesc1.StaticSamplers = GetStaticSamplers();
		_compositionRS = std::make_unique<GDX12RootSignature>(resources->Device, RSDesc1);
	}

	void Execute(GDX12CommandList* cmdList) override
	{
		GDX12Texture* opaqueAccum = IN_OpaqueScene->GetTexture();
		GDX12Texture* transparencyAccum = IN_TransparencyAccum->GetTexture();
		GDX12Texture* transparencyRevealage = IN_Revealage->GetTexture();
		GDX12Texture* output = IN_OUT_Result->GetTexture();

		cmdList->BeginPixEvent("Composition Render Pass", Colors::Bisque);
		cmdList->SetViewport(output->GetViewport());
		cmdList->SetScissorRect(output->GetScissorRect());
		cmdList->SetGraphicsRootSignature(_compositionRS.get());
		cmdList->SetPipelineState(_compositionPSO.Get());
		cmdList->SetRenderTargets({ output }, nullptr);
		cmdList->SetDescriptorHeaps({ _resources->SRV_UAV_Heap.get() });
		cmdList->SetGraphicsSRV(0, opaqueAccum->GetSRV()->GPUHandle);
		cmdList->SetGraphicsSRV(1, transparencyAccum->GetSRV()->GPUHandle);
		cmdList->SetGraphicsSRV(2, transparencyRevealage->GetSRV()->GPUHandle);
		cmdList->GetCommandList()->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		cmdList->GetCommandList()->DrawInstanced(3, 1, 0, 0);
		cmdList->EndPixEvent();
	}
private:
	void PostLinkInitialize()
	{
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
		PSODesc1.RTVFormats[0] = IN_OUT_Result->GetTexture()->GetFormat();

		PSODesc1.SampleDesc.Count = 1;
		PSODesc1.SampleDesc.Quality = 0;
		PSODesc1.DepthStencilState.DepthEnable = false;
		PSODesc1.DepthStencilState.StencilEnable = false;
		PSODesc1.VS = { reinterpret_cast<BYTE*>(_compositionVS->GetBufferPointer()), _compositionVS->GetBufferSize() };
		PSODesc1.PS = { reinterpret_cast<BYTE*>(_compositionPS->GetBufferPointer()), _compositionPS->GetBufferSize() };
		ThrowIfFailed(_resources->Device->GetDevice()->CreateGraphicsPipelineState(&PSODesc1, IID_PPV_ARGS(&_compositionPSO)));
	}

	ComPtr<ID3DBlob> _compositionVS;
	ComPtr<ID3DBlob> _compositionPS;
	std::unique_ptr<GDX12RootSignature> _compositionRS;
	ComPtr<ID3D12PipelineState> _compositionPSO;

	IRenderPassLink* IN_OpaqueScene;
	IRenderPassLink* IN_TransparencyAccum;
	IRenderPassLink* IN_Revealage;
	IRenderPassLink* IN_OUT_Result;
};
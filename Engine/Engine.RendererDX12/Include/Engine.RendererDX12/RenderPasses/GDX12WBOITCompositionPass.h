#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

class GDX12WBOITCompositionPass : public GDX12RenderPass
{
public:
	static constexpr const char* InOpaqueTexture = "InOpaqueTexture";
	static constexpr const char* InAccumulationTexture = "InAccumulationTexture";
	static constexpr const char* InRevealageTexture = "InRevealageTexture";
	static constexpr const char* OutColorTexture = "OutColorTexture";

	void Describe(PassDescription& s) const override
	{
		auto& opaque = s.AddInputTexture(InOpaqueTexture);
		opaque.Formats = {DXGI_FORMAT_R8G8B8A8_UNORM};
		opaque.Usages = ResourceUsage::PixelShaderRead;
		opaque.AllowShared = true;
		auto& accum = s.AddInputTexture(InAccumulationTexture);
		accum.Formats = {DXGI_FORMAT_R8G8B8A8_UNORM};
		accum.Usages = ResourceUsage::PixelShaderRead;
		accum.AllowShared = true;
		auto& revealage = s.AddInputTexture(InRevealageTexture);
		revealage.Formats = {DXGI_FORMAT_R16_FLOAT};
		revealage.Usages = ResourceUsage::PixelShaderRead;
		revealage.AllowShared = true;
		auto& color = s.AddOutputTexture(OutColorTexture);
		color.Formats = {DXGI_FORMAT_R8G8B8A8_UNORM};
		color.Size = Extent::SameAs(InOpaqueTexture);
		color.Usages = ResourceUsage::RenderTarget;
		color.AllowShared = true;
		s.RequireSameTextureSize({InOpaqueTexture, InAccumulationTexture, InRevealageTexture});
	}

	void Initialize(PassInitContext& context) override
	{
		auto* resources = context.GetDeviceResources();
		// Shaders
		auto& shaderCompiler = GDX12ShaderCompiler::GetInstance();
		_compositionVS = shaderCompiler.CompileShader(resources->Device, SHADERS_FOLDER "FullScreenVS.hlsl", nullptr, "VS", "vs");
		_compositionPS = shaderCompiler.CompileShader(resources->Device, SHADERS_FOLDER "CompositionPass.hlsl", nullptr, "PS", "ps");

		// Root Signatures
		GDX12RootSignatureDesc RSDesc1;
		RSDesc1.NumSingleSRVSlots = 3;
		RSDesc1.SingleSRVVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
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
		PSODesc1.RTVFormats[0] = context.GetOutputResourceDescription(OutColorTexture).Format;

		PSODesc1.SampleDesc.Count = 1;
		PSODesc1.SampleDesc.Quality = 0;
		PSODesc1.DepthStencilState.DepthEnable = false;
		PSODesc1.DepthStencilState.StencilEnable = false;
		PSODesc1.VS = { reinterpret_cast<BYTE*>(_compositionVS->GetBufferPointer()), _compositionVS->GetBufferSize() };
		PSODesc1.PS = { reinterpret_cast<BYTE*>(_compositionPS->GetBufferPointer()), _compositionPS->GetBufferSize() };
		ThrowIfFailed(resources->Device->GetDevice()->CreateGraphicsPipelineState(&PSODesc1, IID_PPV_ARGS(&_compositionPSO)));
	}

	void Execute(PassExecutionContext& context) override
	{
		auto* resources = context.GetDeviceResources();
		auto* cmdList = context.GetCommandList();
		auto* OUT_Result = context.GetOutputTexture(OutColorTexture).GetTexture();
		GDX12Texture* opaqueAccum = context.GetInputTexture(InOpaqueTexture).GetTexture();
		GDX12Texture* transparencyAccum = context.GetInputTexture(InAccumulationTexture).GetTexture();
		GDX12Texture* transparencyRevealage = context.GetInputTexture(InRevealageTexture).GetTexture();

		cmdList->BeginPixEvent("Composition Pass", Colors::Gold);
		cmdList->SetViewport(OUT_Result->GetViewport());
		cmdList->SetScissorRect(OUT_Result->GetScissorRect());
		cmdList->SetGraphicsRootSignature(_compositionRS.get());
		cmdList->SetPipelineState(_compositionPSO.Get());
		cmdList->SetDescriptorHeaps({ resources->SRV_UAV_Heap.get() });
		cmdList->SetRenderTargets({ OUT_Result }, nullptr);
		cmdList->ClearRenderTargetView(OUT_Result);
		cmdList->SetGraphicsSRV(0, opaqueAccum->GetSRV()->GPUHandle);
		cmdList->SetGraphicsSRV(1, transparencyAccum->GetSRV()->GPUHandle);
		cmdList->SetGraphicsSRV(2, transparencyRevealage->GetSRV()->GPUHandle);
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

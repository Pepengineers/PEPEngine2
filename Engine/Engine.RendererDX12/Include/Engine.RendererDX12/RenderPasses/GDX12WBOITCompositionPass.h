#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

class GDX12WBOITCompositionPass : public GDX12RenderPass
{
public:
	GDX12WBOITCompositionPass() : IN_OpaqueScene(nullptr), IN_TransparencyAccum(nullptr),
		IN_Revealage(nullptr), IN_DepthStencil(nullptr), IN_VelocityBuffer(nullptr),
		OUT_Result(nullptr), OUT_ResolvedDepth(nullptr), OUT_ResolvedVelocity(nullptr)
	{ 
		_flags = RENDER_PASS_FLAG_NONE; 
		_numInputs = 5;
		_numOutputs = 3;
		OUT_Result = std::make_unique<GDX12Texture>();
		OUT_ResolvedDepth = std::make_unique<GDX12Texture>();
		OUT_ResolvedVelocity = std::make_unique<GDX12Texture>();
		_outputs.push_back(OUT_Result.get());
		_outputs.push_back(OUT_ResolvedDepth.get());
		_outputs.push_back(OUT_ResolvedVelocity.get());
	}

	// Input 0 - OpaqueScene
	// Input 1 - TransparencyAccumulation
	// Input 2 - RevealageTexture
	// Input 3 - HighResDepth
	// Input 4 - HighResVelocity
	// Output 0 - CompositionResult
	// Output 1 - ResolvedDepth
	// Output 2 - ResolvedVelocity
	void Initialize() override
	{
		IN_OpaqueScene = _inputs[0];
		IN_TransparencyAccum = _inputs[1];
		IN_Revealage = _inputs[2];
		IN_DepthStencil = _inputs[3];
		IN_VelocityBuffer = _inputs[4];

		// Textures
		GDX12TextureDesc TextureDesc1;
		TextureDesc1.Format = TextureDesc1.RTVDesc.Format = TextureDesc1.SRVDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		TextureDesc1.Width = GetOutputWidth();
		TextureDesc1.Height = GetOutputHeight();

		TextureDesc1.CreateSRV = true;
		TextureDesc1.SRV_UAV_Heap = _resources->SRV_UAV_Heap.get();
		TextureDesc1.SRVHeapIndex = _resources->SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
		TextureDesc1.SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		TextureDesc1.SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		TextureDesc1.SRVDesc.Texture2D.MipLevels = 1;
		TextureDesc1.SRVDesc.Texture2D.MostDetailedMip = 0;
		TextureDesc1.SRVDesc.Texture2D.PlaneSlice = 0;
		TextureDesc1.SRVDesc.Texture2D.ResourceMinLODClamp = 0.0f;

		TextureDesc1.CreateRTV = true;
		TextureDesc1.RTVHeap = _resources->RTVHeap.get();
		TextureDesc1.RTVHeapIndex = _resources->RTVHeap->GetAvailableIndex();
		TextureDesc1.RTVDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
		TextureDesc1.RTVDesc.Texture2D.PlaneSlice = 0;
		TextureDesc1.RTVDesc.Texture2D.MipSlice = 0;

		OUT_Result->Initialize(TextureDesc1);

		GDX12TextureDesc TextureDesc2;
		TextureDesc2.Format = TextureDesc2.RTVDesc.Format = DXGI_FORMAT_R32_FLOAT;
		TextureDesc2.Width = GetOutputWidth();
		TextureDesc2.Height = GetOutputHeight();
		TextureDesc2.CreateSRV = false;
		TextureDesc2.CreateRTV = true;
		TextureDesc2.RTVHeap = _resources->RTVHeap.get();
		TextureDesc2.RTVHeapIndex = _resources->RTVHeap->GetAvailableIndex();
		TextureDesc2.RTVDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
		TextureDesc2.RTVDesc.Texture2D.PlaneSlice = 0;
		TextureDesc2.RTVDesc.Texture2D.MipSlice = 0;

		OUT_ResolvedDepth->Initialize(TextureDesc2);

		TextureDesc2.Format = TextureDesc2.RTVDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
		TextureDesc2.RTVHeapIndex = _resources->RTVHeap->GetAvailableIndex();

		OUT_ResolvedVelocity->Initialize(TextureDesc2);

		// Shaders
		auto& shaderCompiler = GDX12ShaderCompiler::GetInstance();
		_compositionVS = shaderCompiler.CompileShader(_resources->Device, SHADERS_FOLDER "FullScreenVS.hlsl", nullptr, "VS", "vs");
		_compositionPS = shaderCompiler.CompileShader(_resources->Device, SHADERS_FOLDER "CompositionPass.hlsl", nullptr, "PS", "ps");

		// Root Signatures
		GDX12RootSignatureDesc RSDesc1;
		RSDesc1.NumSingleSRVSlots = 5;
		RSDesc1.Constants.push_back(1);
		RSDesc1.StaticSamplers = GetStaticSamplers();
		_compositionRS = std::make_unique<GDX12RootSignature>(_resources->Device, RSDesc1);

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
		PSODesc1.NumRenderTargets = 3;
		PSODesc1.RTVFormats[0] = OUT_Result->GetFormat();
		PSODesc1.RTVFormats[1] = OUT_ResolvedDepth->GetFormat();
		PSODesc1.RTVFormats[2] = OUT_ResolvedVelocity->GetFormat();

		PSODesc1.SampleDesc.Count = 1;
		PSODesc1.SampleDesc.Quality = 0;
		PSODesc1.DepthStencilState.DepthEnable = false;
		PSODesc1.DepthStencilState.StencilEnable = false;
		PSODesc1.VS = { reinterpret_cast<BYTE*>(_compositionVS->GetBufferPointer()), _compositionVS->GetBufferSize() };
		PSODesc1.PS = { reinterpret_cast<BYTE*>(_compositionPS->GetBufferPointer()), _compositionPS->GetBufferSize() };
		ThrowIfFailed(_resources->Device->GetDevice()->CreateGraphicsPipelineState(&PSODesc1, IID_PPV_ARGS(&_compositionPSO)));
	}

	void Execute(GDX12CommandList* cmdList) override
	{
		GDX12Texture* opaqueAccum = IN_OpaqueScene->GetTexture();
		GDX12Texture* transparencyAccum = IN_TransparencyAccum->GetTexture();
		GDX12Texture* transparencyRevealage = IN_Revealage->GetTexture();
		GDX12Texture* depthStencil = IN_DepthStencil->GetTexture();
		GDX12Texture* velocityBuffer = IN_VelocityBuffer->GetTexture();

		cmdList->BeginPixEvent("Composition Render Pass", Colors::Bisque);
		cmdList->SetViewport(OUT_Result->GetViewport());
		cmdList->SetScissorRect(OUT_Result->GetScissorRect());
		cmdList->SetGraphicsRootSignature(_compositionRS.get());
		cmdList->SetPipelineState(_compositionPSO.Get());
		cmdList->SetDescriptorHeaps({ _resources->SRV_UAV_Heap.get() });
		cmdList->ResourceBarrier({ opaqueAccum->GetResource()->GetSRVBarrier(),
		transparencyAccum->GetResource()->GetSRVBarrier(),
		transparencyRevealage->GetResource()->GetSRVBarrier(),
		depthStencil->GetResource()->GetSRVBarrier(),
		velocityBuffer->GetResource()->GetSRVBarrier(),
		OUT_Result->GetResource()->GetRenderTargetBarrier(),
		OUT_ResolvedDepth->GetResource()->GetRenderTargetBarrier(),
		OUT_ResolvedVelocity->GetResource()->GetRenderTargetBarrier() });
		cmdList->SetRenderTargets({ OUT_Result.get(), OUT_ResolvedDepth.get(), OUT_ResolvedVelocity.get() }, nullptr);
		cmdList->ClearRenderTargetView(OUT_Result.get());
		cmdList->ClearRenderTargetView(OUT_ResolvedDepth.get());
		cmdList->ClearRenderTargetView(OUT_ResolvedVelocity.get());
		cmdList->GetCommandList()->SetGraphicsRoot32BitConstant(_compositionRS->GetBRootParamIndex(0),
			GetSSAAMultiplier(), 0);
		cmdList->SetGraphicsSRV(0, opaqueAccum->GetSRV()->GPUHandle);
		cmdList->SetGraphicsSRV(1, transparencyAccum->GetSRV()->GPUHandle);
		cmdList->SetGraphicsSRV(2, transparencyRevealage->GetSRV()->GPUHandle);
		cmdList->SetGraphicsSRV(3, depthStencil->GetSRV()->GPUHandle);
		cmdList->SetGraphicsSRV(4, velocityBuffer->GetSRV()->GPUHandle);
		cmdList->GetCommandList()->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		cmdList->GetCommandList()->DrawInstanced(3, 1, 0, 0);
		cmdList->EndPixEvent();
	}

	void Resize() override
	{
		UINT newWidth = GetOutputWidth();
		UINT newHeight = GetOutputHeight();
		OUT_Result->Resize(newWidth, newHeight);
		OUT_ResolvedDepth->Resize(newWidth, newHeight);
		OUT_ResolvedVelocity->Resize(newWidth, newHeight);
	}

	void ClearDenendencies() override
	{
		GDX12RenderPass::ClearDenendencies();
		IN_OpaqueScene = nullptr;
		IN_TransparencyAccum = nullptr;
		IN_Revealage = nullptr;
		IN_DepthStencil = nullptr;
		IN_VelocityBuffer = nullptr;
		OUT_Result.reset();
		OUT_ResolvedDepth.reset();
		OUT_ResolvedVelocity.reset();
		_compositionVS.Reset();
		_compositionPS.Reset();
		_compositionRS.reset();
		_compositionPSO.Reset();
	}

private:
	UINT GetOutputWidth()
	{
		return GetFlagValue(RENDER_PASS_FLAG_USE_DOWNSCALED_RESOLUTION) ? _commonData->DownscaledWidth : _commonData->WindowWidth;
	}

	UINT GetOutputHeight()
	{
		return GetFlagValue(RENDER_PASS_FLAG_USE_DOWNSCALED_RESOLUTION) ? _commonData->DownscaledHeight : _commonData->WindowHeight;
	}

	UINT GetSSAAMultiplier()
	{
		return _commonData->SSAAmultiplier > 0 ? _commonData->SSAAmultiplier : 1;
	}

	ComPtr<ID3DBlob> _compositionVS;
	ComPtr<ID3DBlob> _compositionPS;
	std::unique_ptr<GDX12RootSignature> _compositionRS;
	ComPtr<ID3D12PipelineState> _compositionPSO;

	IRenderPassLink* IN_OpaqueScene;
	IRenderPassLink* IN_TransparencyAccum;
	IRenderPassLink* IN_Revealage;
	IRenderPassLink* IN_DepthStencil;
	IRenderPassLink* IN_VelocityBuffer;
	std::unique_ptr<GDX12Texture> OUT_Result;
	std::unique_ptr<GDX12Texture> OUT_ResolvedDepth;
	std::unique_ptr<GDX12Texture> OUT_ResolvedVelocity;
};

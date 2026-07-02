#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

class GDX12WBOITCompositionPass : public GDX12RenderPass
{
public:
	GDX12WBOITCompositionPass() : IN_OpaqueScene(nullptr), IN_TransparencyAccum(nullptr),
		IN_Revealage(nullptr), OUT_Result(nullptr)
	{ _flags = RENDER_PASS_FLAG_NONE; }

	void LinkDependancies(IRenderPassLink* IN_OpaqueScene, IRenderPassLink* IN_TransparencyAccum,
		IRenderPassLink* IN_Revealage, IRenderPassLink*& OUT_Result)
	{
		this->IN_OpaqueScene = IN_OpaqueScene;
		this->IN_TransparencyAccum = IN_TransparencyAccum;
		this->IN_Revealage = IN_Revealage;

		PostLinkInitialize();

		OUT_Result = this->OUT_Result.get();
	}

	void Execute(GDX12CommandList* cmdList) override
	{
		GDX12Texture* opaqueAccum = IN_OpaqueScene->GetTexture();
		GDX12Texture* transparencyAccum = IN_TransparencyAccum->GetTexture();
		GDX12Texture* transparencyRevealage = IN_Revealage->GetTexture();

		cmdList->BeginPixEvent("Composition Render Pass", Colors::Bisque);
		cmdList->SetViewport(OUT_Result->GetViewport());
		cmdList->SetScissorRect(OUT_Result->GetScissorRect());
		cmdList->SetGraphicsRootSignature(_compositionRS.get());
		cmdList->SetPipelineState(_compositionPSO.Get());
		cmdList->SetDescriptorHeaps({ _resources->SRV_UAV_Heap.get() });
		cmdList->ResourceBarrier({ opaqueAccum->GetResource()->GetSRVBarrier(),
		transparencyAccum->GetResource()->GetSRVBarrier(),
		transparencyRevealage->GetResource()->GetSRVBarrier(),
		OUT_Result->GetResource()->GetRenderTargetBarrier() });
		cmdList->SetRenderTargets({ OUT_Result.get() }, nullptr);
		cmdList->ClearRenderTargetView(OUT_Result.get());
		cmdList->SetGraphicsSRV(0, opaqueAccum->GetSRV()->GPUHandle);
		cmdList->SetGraphicsSRV(1, transparencyAccum->GetSRV()->GPUHandle);
		cmdList->SetGraphicsSRV(2, transparencyRevealage->GetSRV()->GPUHandle);
		cmdList->GetCommandList()->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		cmdList->GetCommandList()->DrawInstanced(3, 1, 0, 0);
		cmdList->EndPixEvent();
	}

	void Resize() override
	{
		UINT newWidth = GetFlagValue(RENDER_PASS_FLAG_USE_DOWNSCALED_RESOLUTION) ? _commonData->DownscaledWidth : _commonData->WindowWidth;
		UINT newHeight = GetFlagValue(RENDER_PASS_FLAG_USE_DOWNSCALED_RESOLUTION) ? _commonData->DownscaledHeight : _commonData->WindowHeight;
		OUT_Result->Resize(newWidth, newHeight);
	}
private:
	ComPtr<ID3DBlob> _compositionVS;
	ComPtr<ID3DBlob> _compositionPS;
	std::unique_ptr<GDX12RootSignature> _compositionRS;
	ComPtr<ID3D12PipelineState> _compositionPSO;

	IRenderPassLink* IN_OpaqueScene;
	IRenderPassLink* IN_TransparencyAccum;
	IRenderPassLink* IN_Revealage;
	std::unique_ptr<GDX12Texture> OUT_Result;

	void PostLinkInitialize()
	{
		// Textures
		GDX12TextureDesc TextureDesc1;
		TextureDesc1.Format = TextureDesc1.RTVDesc.Format = TextureDesc1.SRVDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		TextureDesc1.Width = GetFlagValue(RENDER_PASS_FLAG_USE_DOWNSCALED_RESOLUTION) ? _commonData->DownscaledWidth : _commonData->WindowWidth;
		TextureDesc1.Height = GetFlagValue(RENDER_PASS_FLAG_USE_DOWNSCALED_RESOLUTION) ? _commonData->DownscaledHeight : _commonData->WindowHeight;

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

		OUT_Result = std::make_unique<GDX12Texture>(TextureDesc1);

		// Shaders
		auto& shaderCompiler = GDX12ShaderCompiler::GetInstance();
		_compositionVS = shaderCompiler.CompileShader(_resources->Device, SHADERS_FOLDER "FullScreenVS.hlsl", nullptr, "VS", "vs");
		_compositionPS = shaderCompiler.CompileShader(_resources->Device, SHADERS_FOLDER "CompositionPass.hlsl", nullptr, "PS", "ps");

		// Root Signatures
		GDX12RootSignatureDesc RSDesc1;
		RSDesc1.NumSingleSRVSlots = 3;
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
		PSODesc1.NumRenderTargets = 1;
		PSODesc1.RTVFormats[0] = OUT_Result->GetFormat();

		PSODesc1.SampleDesc.Count = 1;
		PSODesc1.SampleDesc.Quality = 0;
		PSODesc1.DepthStencilState.DepthEnable = false;
		PSODesc1.DepthStencilState.StencilEnable = false;
		PSODesc1.VS = { reinterpret_cast<BYTE*>(_compositionVS->GetBufferPointer()), _compositionVS->GetBufferSize() };
		PSODesc1.PS = { reinterpret_cast<BYTE*>(_compositionPS->GetBufferPointer()), _compositionPS->GetBufferSize() };
		ThrowIfFailed(_resources->Device->GetDevice()->CreateGraphicsPipelineState(&PSODesc1, IID_PPV_ARGS(&_compositionPSO)));
	}
};
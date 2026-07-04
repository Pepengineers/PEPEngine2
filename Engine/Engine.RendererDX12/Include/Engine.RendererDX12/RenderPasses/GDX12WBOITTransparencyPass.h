#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

class GDX12WBOITTransparencyPass : public GDX12RenderPass
{
public:
	GDX12WBOITTransparencyPass() : IN_DepthStencil(nullptr)
	{ 
		_flags = RENDER_PASS_FLAG_USE_CAMERAS | RENDER_PASS_FLAG_USE_GEOMETRY | RENDER_PASS_FLAG_USE_MATERIALS | 
		RENDER_PASS_FLAG_USE_INSTANCES; 
		_numInputs = 1;
		_numOutputs = 2;
		OUT_Accumulation = std::make_unique<GDX12Texture>();
		OUT_Revealage = std::make_unique<GDX12Texture>();
		_outputs.push_back(OUT_Accumulation.get());
		_outputs.push_back(OUT_Revealage.get());
	}

	// Input 0 - DepthStencil
	// Output 0 - TransparencyAccumulation
	// Output 1 - RevealageTexture
	void Initialize() override
	{
		IN_DepthStencil = _inputs[0];

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

		OUT_Accumulation->Initialize(TextureDesc1);

		TextureDesc1.Format = TextureDesc1.RTVDesc.Format = TextureDesc1.SRVDesc.Format = DXGI_FORMAT_R16_FLOAT;
		TextureDesc1.SRVHeapIndex = _resources->SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
		TextureDesc1.RTVHeapIndex = _resources->RTVHeap->GetAvailableIndex();

		OUT_Revealage->Initialize(TextureDesc1);

		// Shaders
		auto& shaderCompiler = GDX12ShaderCompiler::GetInstance();
		_transparencyVS = shaderCompiler.CompileShader(_resources->Device, SHADERS_FOLDER "TransparentPass.hlsl", nullptr, "VS", "vs");
		_transparencyPS = shaderCompiler.CompileShader(_resources->Device, SHADERS_FOLDER "TransparentPass.hlsl", nullptr, "PS", "ps");

		// Root Signatures
		GDX12RootSignatureDesc RSDesc1;
		RSDesc1.NumSingleCBVSlots = 2;
		RSDesc1.NumSingleSRVSlots = 3;
		RSDesc1.StaticSamplers = GetStaticSamplers();
		RSDesc1.SRVRanges.push_back(GDX12RootSignatureRange(Texture2D_RangeLength));
		RSDesc1.Constants.push_back(1);
		_transparencyRS = std::make_unique<GDX12RootSignature>(_resources->Device, RSDesc1);

		//Command Signatures
		std::vector<D3D12_INDIRECT_ARGUMENT_DESC> args;
		D3D12_INDIRECT_ARGUMENT_DESC argConst = {};
		argConst.Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
		argConst.Constant.DestOffsetIn32BitValues = 0;
		argConst.Constant.Num32BitValuesToSet = 1;
		args.push_back(argConst);
		D3D12_INDIRECT_ARGUMENT_DESC argDraw = {};
		argDraw.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
		args.push_back(argDraw);

		D3D12_COMMAND_SIGNATURE_DESC CSDesc1 = {};
		CSDesc1.ByteStride = sizeof(GDX12IndirectDrawArgs);
		CSDesc1.NumArgumentDescs = args.size();
		CSDesc1.pArgumentDescs = args.data();
		CSDesc1.NodeMask = 0;

		_resources->Device->GetDevice()->CreateCommandSignature(&CSDesc1,
			_transparencyRS->GetRootSignature().Get(),
			IID_PPV_ARGS(&_transparencyCS));

		// Pipeline State Objects
		D3D12_GRAPHICS_PIPELINE_STATE_DESC PSODesc1 = {};
		PSODesc1.InputLayout = { _resources->InputLayouts["Default"].data(), (UINT)_resources->InputLayouts["Default"].size() };
		PSODesc1.pRootSignature = _transparencyRS->GetRootSignature().Get();
		PSODesc1.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		PSODesc1.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		PSODesc1.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
		//reversed-Z
		PSODesc1.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
		PSODesc1.RasterizerState.FrontCounterClockwise = TRUE;
		PSODesc1.SampleMask = UINT_MAX;
		PSODesc1.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		PSODesc1.SampleDesc.Count = 1;
		PSODesc1.SampleDesc.Quality = 0;
		PSODesc1.DSVFormat = IN_DepthStencil->GetTexture()->GetFormat();

		// Accumulation
		PSODesc1.BlendState.RenderTarget[0].BlendEnable = true;
		PSODesc1.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
		PSODesc1.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
		PSODesc1.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
		PSODesc1.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
		PSODesc1.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
		PSODesc1.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
		PSODesc1.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
		// Revealage
		PSODesc1.BlendState.RenderTarget[1].BlendEnable = true;
		PSODesc1.BlendState.RenderTarget[1].SrcBlend = D3D12_BLEND_ONE;
		PSODesc1.BlendState.RenderTarget[1].DestBlend = D3D12_BLEND_ONE;
		PSODesc1.BlendState.RenderTarget[1].BlendOp = D3D12_BLEND_OP_ADD;
		PSODesc1.BlendState.RenderTarget[1].SrcBlendAlpha = D3D12_BLEND_ONE;
		PSODesc1.BlendState.RenderTarget[1].DestBlendAlpha = D3D12_BLEND_ONE;
		PSODesc1.BlendState.RenderTarget[1].BlendOpAlpha = D3D12_BLEND_OP_ADD;
		PSODesc1.BlendState.RenderTarget[1].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
		// Disable depth write
		PSODesc1.DepthStencilState.DepthEnable = true;
		PSODesc1.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		PSODesc1.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
		PSODesc1.DepthStencilState.StencilEnable = false;

		PSODesc1.NumRenderTargets = 2;
		PSODesc1.RTVFormats[0] = OUT_Accumulation->GetFormat();
		PSODesc1.RTVFormats[1] = OUT_Revealage->GetFormat();
		PSODesc1.VS = { reinterpret_cast<BYTE*>(_transparencyVS->GetBufferPointer()), _transparencyVS->GetBufferSize() };
		PSODesc1.PS = { reinterpret_cast<BYTE*>(_transparencyPS->GetBufferPointer()), _transparencyPS->GetBufferSize() };
		ThrowIfFailed(_resources->Device->GetDevice()->CreateGraphicsPipelineState(&PSODesc1, IID_PPV_ARGS(&_transparencyPSO)));
	}

	// VisibilityBuffers will automatically be selected from CameraCBIndex
	// It requires GPUCullingPass to be executed beforehand
	void Execute(GDX12CommandList* cmdList) override
	{
		auto& currentFrameConstants = _resources->FrameConstants[_resources->CurrFrameConstantsIndex];
		auto& currentCameraVisBuffers = currentFrameConstants->CameraVisibilityCommands[_commonData->ActiveCameraCBufferIndex];
		GDX12Texture* depthStencil = IN_DepthStencil->GetTexture();

		cmdList->BeginPixEvent("Transparent Render Pass", Colors::Aqua);
		cmdList->SetViewport(OUT_Accumulation->GetViewport());
		cmdList->SetScissorRect(OUT_Accumulation->GetScissorRect());
		cmdList->SetGraphicsRootSignature(_transparencyRS.get());
		cmdList->SetPipelineState(_transparencyPSO);
		cmdList->SetGraphicsRootConstantBufferView(1, currentFrameConstants->MainCB->GetElementAddress(0));
		cmdList->SetGraphicsRootConstantBufferView(2, currentFrameConstants->CameraCB->
			GetElementAddress(_commonData->ActiveCameraCBufferIndex));
		cmdList->SetGeometryBuffer(_resources->GeometryBuffer.get());
		cmdList->SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		cmdList->SetDescriptorHeaps({ _resources->SRV_UAV_Heap.get() });
		cmdList->SetGraphicsSRV(0, currentFrameConstants->MaterialCache->GetSRV()->GPUHandle);
		cmdList->SetGraphicsSRV(1, currentFrameConstants->TransformCache->GetSRV()->GPUHandle);
		cmdList->SetGraphicsSRV(2, currentFrameConstants->InstanceCache->GetSRV()->GPUHandle);
		cmdList->SetGraphicsSRV(3, _resources->SRV_UAV_Heap->GetGPUHandle(Texture2D_StartIndex));

		cmdList->ResourceBarrier({ currentCameraVisBuffers.VisibleTransparentCommandsCache->GetResource().GetIndirectArgsBarrier(),
		currentCameraVisBuffers.TransparentDrawCounter->GetResource().GetIndirectArgsBarrier(),
		OUT_Accumulation->GetResource()->GetRenderTargetBarrier(),
		OUT_Revealage->GetResource()->GetRenderTargetBarrier() });
		cmdList->SetRenderTargets({ OUT_Accumulation.get(), OUT_Revealage.get() },
			depthStencil);
		cmdList->ClearRenderTargetView(OUT_Accumulation.get());
		cmdList->ClearRenderTargetView(OUT_Revealage.get());
		cmdList->ExecuteIndirect(_transparencyCS.Get(), _resources->IndirectCommandsCache->GetElementCount(),
			currentCameraVisBuffers.VisibleTransparentCommandsCache->GetResource().D3DResource.Get(), 0,
			currentCameraVisBuffers.TransparentDrawCounter->GetResource().D3DResource.Get(), 0);
		cmdList->ResourceBarrier({ OUT_Accumulation->GetResource()->GetSRVBarrier(),
		OUT_Revealage->GetResource()->GetSRVBarrier() });
		cmdList->EndPixEvent();
	}

	void Resize() override
	{
		UINT newWidth = GetFlagValue(RENDER_PASS_FLAG_USE_DOWNSCALED_RESOLUTION) ? _commonData->DownscaledWidth : _commonData->WindowWidth;
		UINT newHeight = GetFlagValue(RENDER_PASS_FLAG_USE_DOWNSCALED_RESOLUTION) ? _commonData->DownscaledHeight : _commonData->WindowHeight;
		OUT_Accumulation->Resize(newWidth, newHeight);
		OUT_Revealage->Resize(newWidth, newHeight);
	}

	void ClearDenendencies() override
	{
		GDX12RenderPass::ClearDenendencies();
		OUT_Accumulation.reset();
		OUT_Revealage.reset();
		IN_DepthStencil = nullptr;
		_transparencyVS.Reset();
		_transparencyPS.Reset();
		_transparencyRS.reset();
		_transparencyCS.Reset();
		_transparencyPSO.Reset();
	}

private:
	ComPtr<ID3DBlob> _transparencyVS;
	ComPtr<ID3DBlob> _transparencyPS;
	std::unique_ptr<GDX12RootSignature> _transparencyRS;
	ComPtr<ID3D12CommandSignature> _transparencyCS;
	ComPtr<ID3D12PipelineState> _transparencyPSO;

	std::unique_ptr<GDX12Texture> OUT_Accumulation;
	std::unique_ptr<GDX12Texture> OUT_Revealage;

	IRenderPassLink* IN_DepthStencil;
};
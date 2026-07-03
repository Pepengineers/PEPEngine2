#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

class GDX12OpaquePass : public GDX12RenderPass
{
public:
	GDX12OpaquePass() : IN_DepthStencil(nullptr)
	{ _flags = RENDER_PASS_FLAG_USE_CAMERAS | RENDER_PASS_FLAG_USE_GEOMETRY | RENDER_PASS_FLAG_USE_MATERIALS
		| RENDER_PASS_FLAG_USE_INSTANCES; }

	// Input 0 - DepthStencil
	// Output 0 - AccumulationTexture
	// Output 1 - MotionVectors
	void LinkDependancies(std::vector<IRenderPassLink*> inputs, std::vector<IRenderPassLink*>* outputs) override
	{
		IN_DepthStencil = inputs[0];

		PostLinkInitialize();

		outputs->push_back(OUT_Accumulation.get());
		outputs->push_back(OUT_VelocityBuffer.get());
	}

	// VisibilityBuffers will automatically be selected from CameraCBIndex
	// It requires GPUCullingPass to be executed beforehand
	void Execute(GDX12CommandList* cmdList) override
	{
		auto& currentFrameConstants = _resources->FrameConstants[_resources->CurrFrameConstantsIndex];
		auto& currentCameraVisBuffers = currentFrameConstants->CameraVisibilityCommands[_commonData->ActiveCameraCBufferIndex];
		GDX12Texture* depthStencil = IN_DepthStencil->GetTexture();

		cmdList->BeginPixEvent("Opaque Render Pass", Colors::ForestGreen);
		cmdList->SetViewport(OUT_Accumulation->GetViewport());
		cmdList->SetScissorRect(OUT_Accumulation->GetScissorRect());
		cmdList->SetGraphicsRootSignature(_opaqueRS.get());
		cmdList->SetPipelineState(_opaquePSO.Get());
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
		cmdList->ResourceBarrier({ 
			currentCameraVisBuffers.VisibleOpaqueCommandsCache->GetResource().GetIndirectArgsBarrier(),
			currentCameraVisBuffers.OpaqueDrawCounter->GetResource().GetIndirectArgsBarrier(),
			OUT_Accumulation->GetResource()->GetRenderTargetBarrier(),
			OUT_VelocityBuffer->GetResource()->GetRenderTargetBarrier() });
		cmdList->SetRenderTargets({ OUT_Accumulation.get(), OUT_VelocityBuffer.get() }, depthStencil);
		cmdList->ClearRenderTargetView(OUT_Accumulation.get());
		cmdList->ClearRenderTargetView(OUT_VelocityBuffer.get());
		cmdList->ExecuteIndirect(_opaqueCS.Get(), _resources->IndirectCommandsCache->GetElementCount(),
			currentCameraVisBuffers.VisibleOpaqueCommandsCache->GetResource().D3DResource.Get(), 0,
			currentCameraVisBuffers.OpaqueDrawCounter->GetResource().D3DResource.Get(), 0);
		cmdList->EndPixEvent();
	}

	void Resize() override
	{
		UINT newWidth = GetFlagValue(RENDER_PASS_FLAG_USE_DOWNSCALED_RESOLUTION) ? _commonData->DownscaledWidth : _commonData->WindowWidth;
		UINT newHeight = GetFlagValue(RENDER_PASS_FLAG_USE_DOWNSCALED_RESOLUTION) ? _commonData->DownscaledHeight : _commonData->WindowHeight;
		OUT_Accumulation->Resize(newWidth, newHeight);
		OUT_VelocityBuffer->Resize(newWidth, newHeight);
	}

private:
	void PostLinkInitialize()
	{
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

		OUT_Accumulation = std::make_unique<GDX12Texture>(TextureDesc1);

		TextureDesc1.Format = TextureDesc1.RTVDesc.Format = TextureDesc1.SRVDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
		TextureDesc1.RTVHeapIndex = _resources->RTVHeap->GetAvailableIndex();
		TextureDesc1.SRVHeapIndex = _resources->SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);

		OUT_VelocityBuffer = std::make_unique<GDX12Texture>(TextureDesc1);

		// Shaders
		auto& shaderCompiler = GDX12ShaderCompiler::GetInstance();
		_opaqueVS = shaderCompiler.CompileShader(_resources->Device, SHADERS_FOLDER "OpaquePass.hlsl", nullptr, "VS", "vs");
		_opaquePS = shaderCompiler.CompileShader(_resources->Device, SHADERS_FOLDER "OpaquePass.hlsl", nullptr, "PS", "ps");

		// Root Signatures
		GDX12RootSignatureDesc RSDesc1;
		RSDesc1.NumSingleCBVSlots = 2;
		RSDesc1.NumSingleSRVSlots = 3;
		RSDesc1.StaticSamplers = GetStaticSamplers();
		RSDesc1.SRVRanges.push_back(GDX12RootSignatureRange(Texture2D_RangeLength));
		RSDesc1.Constants.push_back(1);
		_opaqueRS = std::make_unique<GDX12RootSignature>(_resources->Device, RSDesc1);

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
			_opaqueRS->GetRootSignature().Get(),
			IID_PPV_ARGS(&_opaqueCS));

		// Pipeline State Objects
		D3D12_GRAPHICS_PIPELINE_STATE_DESC PSODesc1 = {};

		PSODesc1.InputLayout = { _resources->InputLayouts["Default"].data(), (UINT)_resources->InputLayouts["Default"].size() };
		PSODesc1.pRootSignature = _opaqueRS->GetRootSignature().Get();
		PSODesc1.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		PSODesc1.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		PSODesc1.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
		//reversed-Z
		PSODesc1.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
		PSODesc1.RasterizerState.FrontCounterClockwise = TRUE;
		PSODesc1.SampleMask = UINT_MAX;
		PSODesc1.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		PSODesc1.NumRenderTargets = 2;
		PSODesc1.RTVFormats[0] = OUT_Accumulation->GetFormat();
		PSODesc1.RTVFormats[1] = OUT_VelocityBuffer->GetFormat();
		PSODesc1.SampleDesc.Count = 1;
		PSODesc1.SampleDesc.Quality = 0;
		PSODesc1.DSVFormat = IN_DepthStencil->GetTexture()->GetFormat();
		PSODesc1.VS = { reinterpret_cast<BYTE*>(_opaqueVS->GetBufferPointer()), _opaqueVS->GetBufferSize() };
		PSODesc1.PS = { reinterpret_cast<BYTE*>(_opaquePS->GetBufferPointer()), _opaquePS->GetBufferSize() };
		ThrowIfFailed(_resources->Device->GetDevice()->CreateGraphicsPipelineState(&PSODesc1, IID_PPV_ARGS(&_opaquePSO)));
	}

	ComPtr<ID3DBlob> _opaqueVS;
	ComPtr<ID3DBlob> _opaquePS;
	std::unique_ptr<GDX12RootSignature> _opaqueRS;
	ComPtr<ID3D12CommandSignature> _opaqueCS;
	ComPtr<ID3D12PipelineState> _opaquePSO;

	std::unique_ptr<GDX12Texture> OUT_Accumulation;
	std::unique_ptr<GDX12Texture> OUT_VelocityBuffer;

	IRenderPassLink* IN_DepthStencil;
};
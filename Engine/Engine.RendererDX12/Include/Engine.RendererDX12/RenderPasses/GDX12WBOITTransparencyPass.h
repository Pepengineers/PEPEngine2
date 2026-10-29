#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

class GDX12WBOITTransparencyPass : public GDX12RenderPass
{
public:
	static constexpr const char* Draws = "DrawStream";
	static constexpr const char* Depth = "Depth";
	static constexpr const char* Accum = "Accum";
	static constexpr const char* Revealage = "Revealage";

	GDX12WBOITTransparencyPass()
	{
		_flags = RENDER_PASS_FLAG_USE_CAMERAS | RENDER_PASS_FLAG_USE_GEOMETRY |
			RENDER_PASS_FLAG_USE_MATERIALS | RENDER_PASS_FLAG_USE_INSTANCES;
	}

	void Describe(PassDescription& s) const override
	{
		auto& draws = s.AddInputDrawStream(Draws);
		draws.Usages = ResourceUsage::IndirectArguments;
		draws.AllowShared = true;
		auto& depth = s.AddInputTexture(Depth);
		depth.Formats = {DXGI_FORMAT_D32_FLOAT};
		depth.Size = Extent::Render();
		depth.Usages = ResourceUsage::DepthRead;
		auto& accum = s.AddOutputTexture(Accum);
		accum.Formats = {DXGI_FORMAT_R8G8B8A8_UNORM};
		accum.Size = Extent::SameAs(Depth);
		accum.Usages = ResourceUsage::RenderTarget;
		accum.AllowShared = true;
		auto& revealage = s.AddOutputTexture(Revealage);
		revealage.Formats = {DXGI_FORMAT_R16_FLOAT};
		revealage.Size = Extent::SameAs(Depth);
		revealage.Usages = ResourceUsage::RenderTarget;
		revealage.AllowShared = true;
	}

	void Initialize(PassInitContext& context) override
	{
		auto* resources = context.GetDeviceResources();
		// Shaders
		auto& shaderCompiler = GDX12ShaderCompiler::GetInstance();
		_transparencyVS = shaderCompiler.CompileShader(resources->Device, SHADERS_FOLDER "TransparentPass.hlsl", nullptr, "VS", "vs");
		_transparencyPS = shaderCompiler.CompileShader(resources->Device, SHADERS_FOLDER "TransparentPass.hlsl", nullptr, "PS", "ps");

		// Root Signatures
		GDX12RootSignatureDesc RSDesc1;
		RSDesc1.NumSingleCBVSlots = 2;
		RSDesc1.NumSingleSRVSlots = 3;
		RSDesc1.StaticSamplers = GetStaticSamplers();
		RSDesc1.SRVRanges.push_back(GDX12RootSignatureRange(Texture2D_RangeLength));
		RSDesc1.Constants.push_back(1);
		_transparencyRS = std::make_unique<GDX12RootSignature>(resources->Device, RSDesc1);

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

		ThrowIfFailed(resources->Device->GetDevice()->CreateCommandSignature(&CSDesc1,
			_transparencyRS->GetRootSignature().Get(),
			IID_PPV_ARGS(&_transparencyCS)));

		// Pipeline State Objects
		D3D12_GRAPHICS_PIPELINE_STATE_DESC PSODesc1 = {};
		PSODesc1.InputLayout = { resources->InputLayouts["Default"].data(), (UINT)resources->InputLayouts["Default"].size() };
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
		PSODesc1.DSVFormat = context.GetInputResourceDescription(Depth).Format;

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
		PSODesc1.RTVFormats[0] = context.GetOutputResourceDescription(Accum).Format;
		PSODesc1.RTVFormats[1] = context.GetOutputResourceDescription(Revealage).Format;
		PSODesc1.VS = { reinterpret_cast<BYTE*>(_transparencyVS->GetBufferPointer()), _transparencyVS->GetBufferSize() };
		PSODesc1.PS = { reinterpret_cast<BYTE*>(_transparencyPS->GetBufferPointer()), _transparencyPS->GetBufferSize() };
		ThrowIfFailed(resources->Device->GetDevice()->CreateGraphicsPipelineState(&PSODesc1, IID_PPV_ARGS(&_transparencyPSO)));
	}

	void Execute(PassExecutionContext& context) override
	{
		auto* resources = context.GetDeviceResources();
		auto* cmdList = context.GetCommandList();
		const auto* common = context.GetFrameData();
		auto* OUT_Accumulation = context.GetOutputTexture(Accum).GetTexture();
		auto* OUT_Revealage = context.GetOutputTexture(Revealage).GetTexture();
		const auto& draws = context.GetInputDrawStream(Draws);
		auto& currentFrameConstants = resources->FrameConstants[context.GetFrameSlotResourcesIndex()];

		cmdList->BeginPixEvent("Transparent Render Pass", Colors::Aqua);
		cmdList->SetViewport(OUT_Accumulation->GetViewport());
		cmdList->SetScissorRect(OUT_Accumulation->GetScissorRect());
		cmdList->SetGraphicsRootSignature(_transparencyRS.get());
		cmdList->SetPipelineState(_transparencyPSO);
		cmdList->SetGraphicsRootConstantBufferView(1, currentFrameConstants->MainCB->GetElementAddress(0));
		cmdList->SetGraphicsRootConstantBufferView(2, currentFrameConstants->CameraCB->
			GetElementAddress(common->ActiveCameraCBufferIndex));
		cmdList->SetGeometryBuffer(resources->GeometryBuffer.get());
		cmdList->SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		cmdList->SetDescriptorHeaps({ resources->SRV_UAV_Heap.get() });
		cmdList->SetGraphicsSRV(0, currentFrameConstants->MaterialCache->GetSRV()->GPUHandle);
		cmdList->SetGraphicsSRV(1, currentFrameConstants->TransformCache->GetSRV()->GPUHandle);
		cmdList->SetGraphicsSRV(2, currentFrameConstants->InstanceCache->GetSRV()->GPUHandle);
		cmdList->SetGraphicsSRV(3, resources->SRV_UAV_Heap->GetGPUHandle(Texture2D_StartIndex));
		const D3D12_CPU_DESCRIPTOR_HANDLE targets[] = {
			context.GetOutputRTV(Accum)->CPUHandle, context.GetOutputRTV(Revealage)->CPUHandle};
		const auto depth = context.GetInputTexture(Depth).GetDSV()->CPUHandle;
		cmdList->GetCommandList()->OMSetRenderTargets(2, targets, FALSE, &depth);
		cmdList->ClearRenderTargetView(OUT_Accumulation);
		cmdList->ClearRenderTargetView(OUT_Revealage);
		cmdList->ExecuteIndirect(_transparencyCS.Get(), draws.Capacity,
			draws.Arguments->D3DResource.Get(), 0,
			draws.Count->D3DResource.Get(), 0);
		cmdList->EndPixEvent();
	}

private:
	ComPtr<ID3DBlob> _transparencyVS;
	ComPtr<ID3DBlob> _transparencyPS;
	std::unique_ptr<GDX12RootSignature> _transparencyRS;
	ComPtr<ID3D12CommandSignature> _transparencyCS;
	ComPtr<ID3D12PipelineState> _transparencyPSO;

};

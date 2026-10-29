#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

class GDX12OpaquePass : public GDX12RenderPass
{
public:
	static constexpr const char* Draws = "DrawStream";
	static constexpr const char* Color = "Color";
	static constexpr const char* Velocity = "Velocity";
	static constexpr const char* Depth = "Depth";

	GDX12OpaquePass()
	{
		_flags = RENDER_PASS_FLAG_USE_CAMERAS | RENDER_PASS_FLAG_USE_GEOMETRY |
			RENDER_PASS_FLAG_USE_MATERIALS | RENDER_PASS_FLAG_USE_INSTANCES;
	}

	void Describe(PassDescription& s) const override
	{
		auto& draws = s.AddInputDrawStream(Draws);
		draws.Usages = ResourceUsage::IndirectArguments;
		draws.AllowShared = true;
		auto& color = s.AddOutputTexture(Color);
		color.Formats = {DXGI_FORMAT_R8G8B8A8_UNORM};
		color.Size = Extent::Render();
		color.Usages = ResourceUsage::RenderTarget;
		color.AllowShared = true;
		auto& velocity = s.AddOutputTexture(Velocity);
		velocity.Formats = {DXGI_FORMAT_R16G16_FLOAT};
		velocity.Size = Extent::Render();
		velocity.Usages = ResourceUsage::RenderTarget;
		velocity.AllowShared = true;
		auto& depth = s.AddOutputTexture(Depth);
		depth.Formats = {DXGI_FORMAT_D32_FLOAT};
		depth.Size = Extent::Render();
		depth.Usages = ResourceUsage::DepthWrite;
	}

	void Initialize(PassInitContext& context) override
	{
		auto* resources = context.GetDeviceResources();
		// Shaders
		auto& shaderCompiler = GDX12ShaderCompiler::GetInstance();
		_opaqueVS = shaderCompiler.CompileShader(resources->Device, SHADERS_FOLDER "OpaquePass.hlsl", nullptr, "VS", "vs");
		_opaquePS = shaderCompiler.CompileShader(resources->Device, SHADERS_FOLDER "OpaquePass.hlsl", nullptr, "PS", "ps");

		// Root Signatures
		GDX12RootSignatureDesc RSDesc1;
		RSDesc1.NumSingleCBVSlots = 2;
		RSDesc1.NumSingleSRVSlots = 3;
		RSDesc1.StaticSamplers = GetStaticSamplers();
		RSDesc1.SRVRanges.push_back(GDX12RootSignatureRange(Texture2D_RangeLength));
		RSDesc1.Constants.push_back(1);
		_opaqueRS = std::make_unique<GDX12RootSignature>(resources->Device, RSDesc1);

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
			_opaqueRS->GetRootSignature().Get(),
			IID_PPV_ARGS(&_opaqueCS)));

		// Pipeline State Objects
		D3D12_GRAPHICS_PIPELINE_STATE_DESC PSODesc1 = {};

		PSODesc1.InputLayout = { resources->InputLayouts["Default"].data(), (UINT)resources->InputLayouts["Default"].size() };
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
		PSODesc1.RTVFormats[0] = context.GetOutputResourceDescription(Color).Format;
		PSODesc1.RTVFormats[1] = context.GetOutputResourceDescription(Velocity).Format;
		PSODesc1.SampleDesc.Count = 1;
		PSODesc1.SampleDesc.Quality = 0;
		PSODesc1.DSVFormat = context.GetOutputResourceDescription(Depth).Format;
		PSODesc1.VS = { reinterpret_cast<BYTE*>(_opaqueVS->GetBufferPointer()), _opaqueVS->GetBufferSize() };
		PSODesc1.PS = { reinterpret_cast<BYTE*>(_opaquePS->GetBufferPointer()), _opaquePS->GetBufferSize() };
		ThrowIfFailed(resources->Device->GetDevice()->CreateGraphicsPipelineState(&PSODesc1, IID_PPV_ARGS(&_opaquePSO)));
	}

	void Execute(PassExecutionContext& context) override
	{
		auto* resources = context.GetDeviceResources();
		auto* cmdList = context.GetCommandList();
		const auto* common = context.GetFrameData();
		auto* OUT_Accumulation = context.GetOutputTexture(Color).GetTexture();
		auto* OUT_VelocityBuffer = context.GetOutputTexture(Velocity).GetTexture();
		auto* depthStencil = context.GetOutputTexture(Depth).GetTexture();
		const auto& draws = context.GetInputDrawStream(Draws);
		auto& currentFrameConstants = resources->FrameConstants[context.GetFrameSlotResourcesIndex()];

		cmdList->BeginPixEvent("Opaque Render Pass", Colors::ForestGreen);
		cmdList->SetViewport(OUT_Accumulation->GetViewport());
		cmdList->SetScissorRect(OUT_Accumulation->GetScissorRect());
		cmdList->SetGraphicsRootSignature(_opaqueRS.get());
		cmdList->SetPipelineState(_opaquePSO.Get());
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
		cmdList->SetRenderTargets({ OUT_Accumulation, OUT_VelocityBuffer }, depthStencil);
		cmdList->ClearDepthStencilView(depthStencil);
		cmdList->ClearRenderTargetView(OUT_Accumulation);
		cmdList->ClearRenderTargetView(OUT_VelocityBuffer);
		cmdList->ExecuteIndirect(_opaqueCS.Get(), draws.Capacity,
			draws.Arguments->D3DResource.Get(), 0,
			draws.Count->D3DResource.Get(), 0);
		cmdList->EndPixEvent();
	}

private:
	ComPtr<ID3DBlob> _opaqueVS;
	ComPtr<ID3DBlob> _opaquePS;
	std::unique_ptr<GDX12RootSignature> _opaqueRS;
	ComPtr<ID3D12CommandSignature> _opaqueCS;
	ComPtr<ID3D12PipelineState> _opaquePSO;

};

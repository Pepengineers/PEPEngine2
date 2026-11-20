#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

class GDX12DeferredGeometryPass : public GDX12RenderPass
{
public:
	static constexpr const char* InDrawStream = "InDrawStream";
	static constexpr const char* OutAlbedoMetallic = "OutAlbedoMetallic";
	static constexpr const char* OutVelocityBuffer = "OutVelocityBuffer";
	static constexpr const char* OutNormalRoughness = "OutNormalRoughness";
	static constexpr const char* OutF0Baked = "OutF0Baked";
	static constexpr const char* OutEmissive = "OutEmissive";
	static constexpr const char* OutDepthBuffer = "OutDepthBuffer";

	GDX12DeferredGeometryPass()
	{
		_flags = RENDER_PASS_FLAG_USE_CAMERAS | RENDER_PASS_FLAG_USE_GEOMETRY |
			RENDER_PASS_FLAG_USE_MATERIALS | RENDER_PASS_FLAG_USE_INSTANCES;
	}

	void Describe(PassDescription& s) const override
	{
		auto& draws = s.AddInputDrawStream(InDrawStream);
		draws.Usages = ResourceUsage::IndirectArguments;
		draws.AllowShared = true;
		for (const auto& target : ColorTargets)
		{
			auto& output = s.AddOutputTexture(target.Name);
			output.Formats = {target.Format};
			output.Size = Extent::Render();
			output.Usages = ResourceUsage::RenderTarget;
			output.AllowShared = true;
		}
		auto& depth = s.AddOutputTexture(OutDepthBuffer);
		depth.Formats = {DXGI_FORMAT_D32_FLOAT};
		depth.Size = Extent::Render();
		depth.Usages = ResourceUsage::DepthWrite;
	}

	void Initialize(PassInitContext& context) override
	{
		auto* resources = context.GetDeviceResources();
		// Shaders
		auto& shaderCompiler = GDX12ShaderCompiler::GetInstance();
		auto vs = shaderCompiler.CompileShader(resources->Device, SHADERS_FOLDER "DeferredGeometryPass.hlsl", nullptr, "VS", "vs");
		auto ps = shaderCompiler.CompileShader(resources->Device, SHADERS_FOLDER "DeferredGeometryPass.hlsl", nullptr, "PS", "ps");
		auto depthPS = shaderCompiler.CompileShader(resources->Device, SHADERS_FOLDER "DeferredGeometryPass.hlsl", nullptr, "DepthPS", "ps");

		// Root Signatures
		GDX12RootSignatureDesc rootDesc;
		rootDesc.NumSingleCBVSlots = 2;
		rootDesc.NumSingleSRVSlots = 3;
		rootDesc.StaticSamplers = GetStaticSamplers();
		rootDesc.SRVRanges.push_back(GDX12RootSignatureRange(Texture2D_RangeLength));
		rootDesc.Constants.push_back(1);
		_opaqueRS = std::make_unique<GDX12RootSignature>(resources->Device, rootDesc);

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

		D3D12_COMMAND_SIGNATURE_DESC signatureDesc = {};
		signatureDesc.ByteStride = sizeof(GDX12IndirectDrawArgs);
		signatureDesc.NumArgumentDescs = args.size();
		signatureDesc.pArgumentDescs = args.data();
		signatureDesc.NodeMask = 0;

		ThrowIfFailed(resources->Device->GetDevice()->CreateCommandSignature(&signatureDesc,
			_opaqueRS->GetRootSignature().Get(),
			IID_PPV_ARGS(&_opaqueCS)));

		// Pipeline State Objects
		D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};

		pso.InputLayout = { resources->InputLayouts["Default"].data(), (UINT)resources->InputLayouts["Default"].size() };
		pso.pRootSignature = _opaqueRS->GetRootSignature().Get();
		pso.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		pso.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		pso.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
		//reversed-Z
		pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
		pso.RasterizerState.FrontCounterClockwise = TRUE;
		pso.SampleMask = UINT_MAX;
		pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		pso.SampleDesc.Count = 1;
		pso.SampleDesc.Quality = 0;
		pso.DSVFormat = context.GetOutputResourceDescription(OutDepthBuffer).Format;
		pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
		pso.PS = {depthPS->GetBufferPointer(), depthPS->GetBufferSize()};
		ThrowIfFailed(resources->Device->GetDevice()->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&_depthPSO)));

		pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_EQUAL;
		pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		pso.NumRenderTargets = static_cast<UINT>(std::size(ColorTargets));
		for (UINT i = 0; i < pso.NumRenderTargets; ++i)
		{
			pso.RTVFormats[i] = ColorTargets[i].Format;
		}
		pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
		ThrowIfFailed(resources->Device->GetDevice()->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&_opaquePSO)));
	}

	void Execute(PassExecutionContext& context) override
	{
		auto* resources = context.GetDeviceResources();
		auto* cmdList = context.GetCommandList();
		const auto* common = context.GetFrameData();
		auto* albedoMetallic = context.GetOutputTexture(OutAlbedoMetallic).GetTexture();
		auto* velocity = context.GetOutputTexture(OutVelocityBuffer).GetTexture();
		auto* depthStencil = context.GetOutputTexture(OutDepthBuffer).GetTexture();
		const auto& draws = context.GetInputDrawStream(InDrawStream);
		auto& currentFrameConstants = resources->FrameConstants[context.GetFrameSlotResourcesIndex()];

		cmdList->BeginPixEvent("Depth Prepass", Colors::SteelBlue);
		cmdList->SetViewport(albedoMetallic->GetViewport());
		cmdList->SetScissorRect(albedoMetallic->GetScissorRect());
		cmdList->SetGraphicsRootSignature(_opaqueRS.get());
		cmdList->SetPipelineState(_depthPSO.Get());
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
		auto drawVisibleMeshes = [&]()
		{
			cmdList->ExecuteIndirect(_opaqueCS.Get(), draws.Capacity,
				draws.Arguments->D3DResource.Get(), 0, draws.Count->D3DResource.Get(), 0);
		};
		cmdList->SetRenderTargets({}, depthStencil);
		cmdList->ClearDepthStencilView(depthStencil);
		drawVisibleMeshes();
		cmdList->EndPixEvent();

		cmdList->BeginPixEvent("G-buffer", Colors::ForestGreen);
		cmdList->SetPipelineState(_opaquePSO.Get());
		const auto targets = {albedoMetallic, velocity, context.GetOutputTexture(OutNormalRoughness).GetTexture(),
			context.GetOutputTexture(OutF0Baked).GetTexture(), context.GetOutputTexture(OutEmissive).GetTexture()};
		cmdList->SetRenderTargets(targets, depthStencil);
		for (auto* target : targets) { cmdList->ClearRenderTargetView(target); }
		drawVisibleMeshes();
		cmdList->EndPixEvent();
	}

private:
	// MRT order matches PS_OUTPUT in DeferredGeometryPass.hlsl.
	struct ColorTarget { const char* Name; DXGI_FORMAT Format; };
	static constexpr ColorTarget ColorTargets[] = {
		{OutAlbedoMetallic, DXGI_FORMAT_R8G8B8A8_UNORM},
		{OutVelocityBuffer, DXGI_FORMAT_R16G16_FLOAT},
		{OutNormalRoughness, DXGI_FORMAT_R16G16B16A16_FLOAT},
		{OutF0Baked, DXGI_FORMAT_R16G16B16A16_FLOAT},
		{OutEmissive, DXGI_FORMAT_R16G16B16A16_FLOAT}
	};
	ComPtr<ID3D12PipelineState> _depthPSO;
	std::unique_ptr<GDX12RootSignature> _opaqueRS;
	ComPtr<ID3D12CommandSignature> _opaqueCS;
	ComPtr<ID3D12PipelineState> _opaquePSO;

};

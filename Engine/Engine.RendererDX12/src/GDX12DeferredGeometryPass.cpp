#include "Engine.RendererDX12/RenderPasses/GDX12DeferredGeometryPass.h"

GDX12DeferredGeometryPass::GDX12DeferredGeometryPass()
{
	_flags = RENDER_PASS_FLAG_USE_CAMERAS | RENDER_PASS_FLAG_USE_GEOMETRY |
		RENDER_PASS_FLAG_USE_MATERIALS | RENDER_PASS_FLAG_USE_INSTANCES;
}

void GDX12DeferredGeometryPass::Describe(PassDescription& s) const
{
	auto& draws = s.AddInputDrawStream(InDrawStream);
	draws.Usages = ResourceUsage::IndirectArguments;
	draws.AllowShared = true;

	auto& albedoMetallic = s.AddOutputTexture(OutAlbedoMetallic);
	albedoMetallic.Formats = {DXGI_FORMAT_R8G8B8A8_UNORM};
	albedoMetallic.Size = Extent::Render();
	albedoMetallic.Usages = ResourceUsage::RenderTarget;
	albedoMetallic.AllowShared = true;

	auto& velocity = s.AddOutputTexture(OutVelocityBuffer);
	velocity.Formats = {DXGI_FORMAT_R16G16_FLOAT};
	velocity.Size = Extent::Render();
	velocity.Usages = ResourceUsage::RenderTarget;
	velocity.AllowShared = true;

	auto& normalRoughness = s.AddOutputTexture(OutNormalRoughness);
	normalRoughness.Formats = {DXGI_FORMAT_R16G16B16A16_FLOAT};
	normalRoughness.Size = Extent::Render();
	normalRoughness.Usages = ResourceUsage::RenderTarget;
	normalRoughness.AllowShared = true;

	auto& f0Baked = s.AddOutputTexture(OutF0Baked);
	f0Baked.Formats = {DXGI_FORMAT_R16G16B16A16_FLOAT};
	f0Baked.Size = Extent::Render();
	f0Baked.Usages = ResourceUsage::RenderTarget;
	f0Baked.AllowShared = true;

	auto& emissive = s.AddOutputTexture(OutEmissive);
	emissive.Formats = {DXGI_FORMAT_R16G16B16A16_FLOAT};
	emissive.Size = Extent::Render();
	emissive.Usages = ResourceUsage::RenderTarget;
	emissive.AllowShared = true;

	auto& depth = s.AddOutputTexture(OutDepthBuffer);
	depth.Formats = {DXGI_FORMAT_D32_FLOAT};
	depth.Size = Extent::Render();
	depth.Usages = ResourceUsage::DepthWrite;
}

void GDX12DeferredGeometryPass::Initialize(PassInitContext& context)
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
	// MRT order matches PS_OUTPUT in DeferredGeometryPass.hlsl.
	pso.NumRenderTargets = 5;
	pso.RTVFormats[0] = context.GetOutputResourceDescription(OutAlbedoMetallic).Format;
	pso.RTVFormats[1] = context.GetOutputResourceDescription(OutVelocityBuffer).Format;
	pso.RTVFormats[2] = context.GetOutputResourceDescription(OutNormalRoughness).Format;
	pso.RTVFormats[3] = context.GetOutputResourceDescription(OutF0Baked).Format;
	pso.RTVFormats[4] = context.GetOutputResourceDescription(OutEmissive).Format;
	pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
	ThrowIfFailed(resources->Device->GetDevice()->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&_opaquePSO)));
}

void GDX12DeferredGeometryPass::Execute(PassExecutionContext& context)
{
	auto* resources = context.GetDeviceResources();
	auto* cmdList = context.GetCommandList();
	const auto* common = context.GetFrameData();
	auto* albedoMetallic = context.GetOutputTexture(OutAlbedoMetallic).GetTexture();
	auto* velocity = context.GetOutputTexture(OutVelocityBuffer).GetTexture();
	auto* depthStencil = context.GetOutputTexture(OutDepthBuffer).GetTexture();
	const auto* draws = context.GetInputDrawStream(InDrawStream);
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
		cmdList->ExecuteIndirect(_opaqueCS.Get(), draws->Capacity,
			draws->Arguments->D3DResource.Get(), 0, draws->Count->D3DResource.Get(), 0);
	};
	cmdList->SetRenderTargets({}, depthStencil);
	cmdList->ClearDepthStencilView(depthStencil);
	drawVisibleMeshes();
	cmdList->EndPixEvent();

	cmdList->BeginPixEvent("G-buffer", Colors::ForestGreen);
	cmdList->SetPipelineState(_opaquePSO.Get());
	auto* normalRoughness = context.GetOutputTexture(OutNormalRoughness).GetTexture();
	auto* f0Baked = context.GetOutputTexture(OutF0Baked).GetTexture();
	auto* emissive = context.GetOutputTexture(OutEmissive).GetTexture();
	cmdList->SetRenderTargets({albedoMetallic, velocity, normalRoughness, f0Baked, emissive}, depthStencil);
	cmdList->ClearRenderTargetView(albedoMetallic);
	cmdList->ClearRenderTargetView(velocity);
	cmdList->ClearRenderTargetView(normalRoughness);
	cmdList->ClearRenderTargetView(f0Baked);
	cmdList->ClearRenderTargetView(emissive);
	drawVisibleMeshes();
	cmdList->EndPixEvent();
}

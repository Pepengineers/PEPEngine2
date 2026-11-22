#include "Engine.RendererDX12/RenderPasses/GDX12WBOITTransparencyPass.h"

GDX12WBOITTransparencyPass::GDX12WBOITTransparencyPass()
{
	_flags = RENDER_PASS_FLAG_USE_CAMERAS | RENDER_PASS_FLAG_USE_GEOMETRY |
		RENDER_PASS_FLAG_USE_LIGHTING | RENDER_PASS_FLAG_USE_MATERIALS | RENDER_PASS_FLAG_USE_INSTANCES;
}

void GDX12WBOITTransparencyPass::Describe(PassDescription& s) const
{
	auto& draws = s.AddInputDrawStream(InDrawStream);
	draws.Usages = ResourceUsage::IndirectArguments;
	draws.AllowShared = true;
	auto& depth = s.AddInputTexture(InDepthBuffer);
	depth.Formats = {DXGI_FORMAT_D32_FLOAT};
	depth.Size = Extent::Render();
	depth.Usages = ResourceUsage::DepthRead;
	// These SRV tables use a graphics root signature with visibility ALL.
	s.AddInputTexture("InIrradiance", ResourceType::TextureCube).Usages = ResourceUsage::GraphicsShaderRead;
	s.AddInputTexture("InPrefilteredEnvironment", ResourceType::TextureCube).Usages = ResourceUsage::GraphicsShaderRead;
	s.AddInputTexture("InBRDFLut", ResourceType::Texture2D).Usages = ResourceUsage::GraphicsShaderRead;

	auto& directionalMaps = s.AddInputTexture("InDirectionalMaps", ResourceType::Texture2DArray);
	directionalMaps.Formats = {DXGI_FORMAT_D32_FLOAT};
	directionalMaps.Usages = ResourceUsage::GraphicsShaderRead;

	auto& pointSpotMaps = s.AddInputTexture("InPointSpotMaps", ResourceType::Texture2DArray);
	pointSpotMaps.Formats = {DXGI_FORMAT_D32_FLOAT};
	pointSpotMaps.Usages = ResourceUsage::GraphicsShaderRead;

	auto& accum = s.AddOutputTexture(OutAccumulationTexture);
	accum.Formats = {DXGI_FORMAT_R16G16B16A16_FLOAT};
	accum.ClearColor = {0, 0, 0, 0};
	accum.Size = Extent::SameAs(InDepthBuffer);
	accum.Usages = ResourceUsage::RenderTarget;
	accum.AllowShared = true;
	auto& revealage = s.AddOutputTexture(OutRevealageTexture);
	revealage.Formats = {DXGI_FORMAT_R16_FLOAT};
	revealage.ClearColor = {1, 1, 1, 1};
	revealage.Size = Extent::SameAs(InDepthBuffer);
	revealage.Usages = ResourceUsage::RenderTarget;
	revealage.AllowShared = true;
}

void GDX12WBOITTransparencyPass::Initialize(PassInitContext& context)
{
	auto* resources = context.GetDeviceResources();
	// Shaders
	auto& shaderCompiler = GDX12ShaderCompiler::GetInstance();
	_transparencyVS = shaderCompiler.CompileShader(resources->Device, SHADERS_FOLDER "TransparentPass.hlsl", nullptr, "VS", "vs");
	_transparencyPS = shaderCompiler.CompileShader(resources->Device, SHADERS_FOLDER "TransparentPass.hlsl", nullptr, "PS", "ps");

	// Root Signatures
	GDX12RootSignatureDesc RSDesc1;
	RSDesc1.NumSingleCBVSlots = 1;
	RSDesc1.NumSingleSRVSlots = 11;
	RSDesc1.StaticSamplers = GetStaticSamplers();
	CD3DX12_STATIC_SAMPLER_DESC shadowSampler(6, D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT);
	shadowSampler.AddressU = shadowSampler.AddressV = shadowSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
	shadowSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
	shadowSampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
	RSDesc1.StaticSamplers.push_back(shadowSampler);
	RSDesc1.SRVRanges.push_back(GDX12RootSignatureRange(Texture2D_RangeLength));
	RSDesc1.Constants = {1, sizeof(GDX12LightGridConstants) / sizeof(UINT)};
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
	PSODesc1.BlendState.IndependentBlendEnable = TRUE;
	PSODesc1.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	//reversed-Z
	PSODesc1.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
	PSODesc1.RasterizerState.FrontCounterClockwise = TRUE;
	PSODesc1.SampleMask = UINT_MAX;
	PSODesc1.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	PSODesc1.SampleDesc.Count = 1;
	PSODesc1.SampleDesc.Quality = 0;
	PSODesc1.DSVFormat = context.GetInputResourceDescription(InDepthBuffer).Format;

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
	PSODesc1.BlendState.RenderTarget[1].SrcBlend = D3D12_BLEND_ZERO;
	PSODesc1.BlendState.RenderTarget[1].DestBlend = D3D12_BLEND_INV_SRC_COLOR;
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
	PSODesc1.RTVFormats[0] = context.GetOutputResourceDescription(OutAccumulationTexture).Format;
	PSODesc1.RTVFormats[1] = context.GetOutputResourceDescription(OutRevealageTexture).Format;
	PSODesc1.VS = { reinterpret_cast<BYTE*>(_transparencyVS->GetBufferPointer()), _transparencyVS->GetBufferSize() };
	PSODesc1.PS = { reinterpret_cast<BYTE*>(_transparencyPS->GetBufferPointer()), _transparencyPS->GetBufferSize() };
	ThrowIfFailed(resources->Device->GetDevice()->CreateGraphicsPipelineState(&PSODesc1, IID_PPV_ARGS(&_transparencyPSO)));
}

void GDX12WBOITTransparencyPass::Execute(PassExecutionContext& context)
{
	auto* resources = context.GetDeviceResources();
	auto* cmdList = context.GetCommandList();
	const auto* common = context.GetFrameData();
	auto* OUT_Accumulation = context.GetOutputTexture(OutAccumulationTexture).GetTexture();
	auto* OUT_Revealage = context.GetOutputTexture(OutRevealageTexture).GetTexture();
	const auto& draws = context.GetInputDrawStream(InDrawStream);
	auto& currentFrameConstants = resources->FrameConstants[context.GetFrameSlotResourcesIndex()];

	cmdList->BeginPixEvent("Transparency Pass", Colors::Aqua);
	cmdList->SetViewport(OUT_Accumulation->GetViewport());
	cmdList->SetScissorRect(OUT_Accumulation->GetScissorRect());
	cmdList->SetGraphicsRootSignature(_transparencyRS.get());
	cmdList->SetPipelineState(_transparencyPSO);
	const auto& grid = currentFrameConstants->ClusteredLightingConstants;
	cmdList->GetCommandList()->SetGraphicsRoot32BitConstants(1, sizeof(grid) / sizeof(UINT), &grid, 0);
	cmdList->SetGraphicsRootConstantBufferView(2, currentFrameConstants->CameraCB->
		GetElementAddress(common->ActiveCameraCBufferIndex));
	cmdList->SetGeometryBuffer(resources->GeometryBuffer.get());
	cmdList->SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	cmdList->SetDescriptorHeaps({ resources->SRV_UAV_Heap.get() });
	cmdList->SetGraphicsSRV(0, currentFrameConstants->MaterialCache->GetSRV()->GPUHandle);
	cmdList->SetGraphicsSRV(1, currentFrameConstants->TransformCache->GetSRV()->GPUHandle);
	cmdList->SetGraphicsSRV(2, currentFrameConstants->InstanceCache->GetSRV()->GPUHandle);
	cmdList->SetGraphicsSRV(3, context.GetInputTexture("InIrradiance").GetSRV()->GPUHandle);
	cmdList->SetGraphicsSRV(4, context.GetInputTexture("InPrefilteredEnvironment").GetSRV()->GPUHandle);
	cmdList->SetGraphicsSRV(5, context.GetInputTexture("InBRDFLut").GetSRV()->GPUHandle);
	cmdList->SetGraphicsSRV(6, currentFrameConstants->LightsCache->GetSRV()->GPUHandle);
	cmdList->SetGraphicsSRV(7, currentFrameConstants->ClusterLightMaskBuffer->GetSRV()->GPUHandle);
	cmdList->SetGraphicsSRV(8, context.GetInputTexture("InDirectionalMaps").GetSRV()->GPUHandle);
	cmdList->SetGraphicsSRV(9, context.GetInputTexture("InPointSpotMaps").GetSRV()->GPUHandle);
	cmdList->SetGraphicsSRV(10, currentFrameConstants->ShadowViews->GetSRV()->GPUHandle);
	cmdList->SetGraphicsSRV(11, resources->SRV_UAV_Heap->GetGPUHandle(Texture2D_StartIndex));
	const D3D12_CPU_DESCRIPTOR_HANDLE targets[] = {
		context.GetOutputRTV(OutAccumulationTexture)->CPUHandle, context.GetOutputRTV(OutRevealageTexture)->CPUHandle};
	const auto depth = context.GetInputTexture(InDepthBuffer).GetDSV()->CPUHandle;
	cmdList->GetCommandList()->OMSetRenderTargets(2, targets, FALSE, &depth);
	cmdList->ClearRenderTargetView(OUT_Accumulation);
	cmdList->ClearRenderTargetView(OUT_Revealage);
	cmdList->ExecuteIndirect(_transparencyCS.Get(), draws.Capacity,
		draws.Arguments->D3DResource.Get(), 0,
		draws.Count->D3DResource.Get(), 0);
	cmdList->EndPixEvent();
}

#include "Engine.RendererDX12/RenderPasses/GDX12FrustumCullingPass.h"

GDX12FrustumCullingPass::GDX12FrustumCullingPass()
{ _flags = RENDER_PASS_FLAG_USE_CAMERAS | RENDER_PASS_FLAG_USE_INSTANCES |
	RENDER_PASS_FLAG_USE_MATERIALS | RENDER_PASS_FLAG_USE_GEOMETRY; }

void GDX12FrustumCullingPass::Describe(PassDescription& description) const
{
	auto& opaqueDrawStream = description.AddOutputDrawStream(OutOpaqueDrawStream);
	opaqueDrawStream.Usages = ResourceUsage::UnorderedAccess;
	opaqueDrawStream.AllowShared = true;
	auto& transparentDrawStream = description.AddOutputDrawStream(OutTransparentDrawStream);
	transparentDrawStream.Usages = ResourceUsage::UnorderedAccess;
	transparentDrawStream.AllowShared = true;
}

void GDX12FrustumCullingPass::ResolveResourceRequirements(PassResourceRequirementsContext& context)
{
	const auto& sceneDrawCommands = context.GetDeviceResources()->IndirectCommandsCache;
	if (!sceneDrawCommands) { throw std::invalid_argument("ERROR: GPU culling has no scene indirect-command buffer. Initialize IndirectCommandsCache on the pass device "
		"before compiling the graph."); }
	const auto drawStreamCapacity = (std::max)(1u, sceneDrawCommands->GetElementCount());
	context.SetOutputBufferCapacity(OutOpaqueDrawStream, drawStreamCapacity);
	context.SetOutputBufferCapacity(OutTransparentDrawStream, drawStreamCapacity);
}

void GDX12FrustumCullingPass::Initialize(PassInitContext& context)
{
	auto* deviceResources = context.GetDeviceResources();
	// Shaders
	auto& shaderCompiler = GDX12ShaderCompiler::GetInstance();
	_frustumCullingShader = shaderCompiler.CompileShader(deviceResources->Device, SHADERS_FOLDER "FrustumCullingPass.hlsl", nullptr, "CS", "cs");
	_clearDrawCountsShader = shaderCompiler.CompileShader(deviceResources->Device, SHADERS_FOLDER "BufferClear.hlsl", nullptr, "CS", "cs");

	// Root Signatures
	GDX12RootSignatureDesc frustumCullingRootDescription;
	frustumCullingRootDescription.NumSingleCBVSlots = 1;
	frustumCullingRootDescription.NumSingleSRVSlots = 3;
	frustumCullingRootDescription.NumSingleUAVSlots = 4;
	_frustumCullingRootSignature = std::make_unique<GDX12RootSignature>(deviceResources->Device, frustumCullingRootDescription);

	GDX12RootSignatureDesc clearDrawCountsRootDescription;
	clearDrawCountsRootDescription.NumSingleUAVSlots = 2;
	_clearDrawCountsRootSignature = std::make_unique<GDX12RootSignature>(deviceResources->Device, clearDrawCountsRootDescription);

	// Pipeline State Objects
	D3D12_COMPUTE_PIPELINE_STATE_DESC frustumCullingPipelineDescription = {};
	frustumCullingPipelineDescription.pRootSignature = _frustumCullingRootSignature->GetRootSignature().Get();
	frustumCullingPipelineDescription.CS = { reinterpret_cast<BYTE*>(_frustumCullingShader->GetBufferPointer()), _frustumCullingShader->GetBufferSize() };

	ThrowIfFailed(deviceResources->Device->GetDevice()->CreateComputePipelineState(
		&frustumCullingPipelineDescription, IID_PPV_ARGS(&_frustumCullingPipeline)));

	D3D12_COMPUTE_PIPELINE_STATE_DESC clearDrawCountsPipelineDescription = {};
	clearDrawCountsPipelineDescription.pRootSignature = _clearDrawCountsRootSignature->GetRootSignature().Get();
	clearDrawCountsPipelineDescription.CS = { reinterpret_cast<BYTE*>(_clearDrawCountsShader->GetBufferPointer()), _clearDrawCountsShader->GetBufferSize() };

	ThrowIfFailed(deviceResources->Device->GetDevice()->CreateComputePipelineState(
		&clearDrawCountsPipelineDescription, IID_PPV_ARGS(&_clearDrawCountsPipeline)));
}

void GDX12FrustumCullingPass::Execute(PassExecutionContext& context)
{
	const auto& visibleOpaqueDraws = context.GetOutputDrawStream(OutOpaqueDrawStream);
	const auto& visibleTransparentDraws = context.GetOutputDrawStream(OutTransparentDrawStream);
	auto* deviceResources = context.GetDeviceResources();
	auto* commandList = context.GetCommandList();
	const auto* frameData = context.GetFrameData();
	auto& frameConstants = deviceResources->FrameConstants[context.GetFrameSlotResourcesIndex()];

	commandList->BeginPixEvent("Frustum Culling", Colors::CornflowerBlue);

	commandList->SetComputeRootSignature(_clearDrawCountsRootSignature.get());
	commandList->SetPipelineState(_clearDrawCountsPipeline);
	commandList->SetDescriptorHeaps({ deviceResources->SRV_UAV_Heap.get() });
	commandList->SetComputeUAV(0, visibleOpaqueDraws.CountUAV->GPUHandle);
	commandList->SetComputeUAV(1, visibleTransparentDraws.CountUAV->GPUHandle);
	commandList->Dispatch(1, 1, 1);
	commandList->ResourceBarrier({ visibleOpaqueDraws.Count->GetUAVBarrier(), visibleTransparentDraws.Count->GetUAVBarrier() });

	commandList->SetComputeRootSignature(_frustumCullingRootSignature.get());
	commandList->SetPipelineState(_frustumCullingPipeline);
	commandList->SetDescriptorHeaps({ deviceResources->SRV_UAV_Heap.get() });
	commandList->SetComputeRootConstantBufferView(0, frameConstants->CameraCB->GetElementAddress(frameData->ActiveCameraCBufferIndex));
	commandList->SetComputeSRV(0, frameConstants->InstanceCache->GetSRV()->GPUHandle);
	commandList->SetComputeSRV(1, deviceResources->IndirectCommandsCache->GetSRV()->GPUHandle);
	commandList->SetComputeSRV(2, frameConstants->MaterialCache->GetSRV()->GPUHandle);
	commandList->SetComputeUAV(0, visibleOpaqueDraws.ArgumentUAV->GPUHandle);
	commandList->SetComputeUAV(1, visibleOpaqueDraws.CountUAV->GPUHandle);
	commandList->SetComputeUAV(2, visibleTransparentDraws.ArgumentUAV->GPUHandle);
	commandList->SetComputeUAV(3, visibleTransparentDraws.CountUAV->GPUHandle);
	const UINT sceneDrawCount = deviceResources->IndirectCommandsCache->GetElementCount();
	if (sceneDrawCount) { commandList->Dispatch((sceneDrawCount + 63) / 64, 1, 1); }

	commandList->ResourceBarrier({ visibleOpaqueDraws.Arguments->GetUAVBarrier(), visibleOpaqueDraws.Count->GetUAVBarrier(),
		visibleTransparentDraws.Arguments->GetUAVBarrier(), visibleTransparentDraws.Count->GetUAVBarrier() });
	commandList->EndPixEvent();
}

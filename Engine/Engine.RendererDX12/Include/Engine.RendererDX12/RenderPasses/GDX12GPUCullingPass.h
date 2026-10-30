#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

class GDX12GPUCullingPass : public GDX12RenderPass
{
public:
	static constexpr const char* OutOpaqueDrawStream = "OutOpaqueDrawStream";
	static constexpr const char* OutTransparentDrawStream = "OutTransparentDrawStream";

	GDX12GPUCullingPass()
	{ _flags = RENDER_PASS_FLAG_USE_CAMERAS | RENDER_PASS_FLAG_USE_INSTANCES |
		RENDER_PASS_FLAG_USE_MATERIALS | RENDER_PASS_FLAG_USE_GEOMETRY; }

	void Describe(PassDescription& s) const override
	{
		auto& opaqueDrawStream = s.AddOutputDrawStream(OutOpaqueDrawStream);
		opaqueDrawStream.Usages = ResourceUsage::UnorderedAccess;
		opaqueDrawStream.AllowShared = true;
		auto& transparentDrawStream = s.AddOutputDrawStream(OutTransparentDrawStream);
		transparentDrawStream.Usages = ResourceUsage::UnorderedAccess;
		transparentDrawStream.AllowShared = true;
	}

	void ResolveResourceRequirements(PassResourceRequirementsContext& context) override
	{
		const auto& commands = context.GetDeviceResources()->IndirectCommandsCache;
		if (!commands) { throw std::invalid_argument("ERROR: GPU culling has no scene indirect-command buffer. Initialize IndirectCommandsCache on the pass device "
			"before compiling the graph."); }
		const auto capacity = (std::max)(1u, commands->GetElementCount());
		context.SetOutputBufferCapacity(OutOpaqueDrawStream, capacity);
		context.SetOutputBufferCapacity(OutTransparentDrawStream, capacity);
	}

	void Initialize(PassInitContext& context) override
	{
		auto* resources = context.GetDeviceResources();
		// Shaders
		auto& shaderCompiler = GDX12ShaderCompiler::GetInstance();
		_cullingCS = shaderCompiler.CompileShader(resources->Device, SHADERS_FOLDER "Culling.hlsl", nullptr, "CS", "cs");
		_bufferClearCS = shaderCompiler.CompileShader(resources->Device, SHADERS_FOLDER "BufferClear.hlsl", nullptr, "CS", "cs");

		// Root Signatures
		GDX12RootSignatureDesc RSDesc1;
		RSDesc1.NumSingleCBVSlots = 1;
		RSDesc1.NumSingleSRVSlots = 3;
		RSDesc1.NumSingleUAVSlots = 4;
		_cullingRS = std::make_unique<GDX12RootSignature>(resources->Device, RSDesc1);

		GDX12RootSignatureDesc RSDesc2;
		RSDesc2.NumSingleUAVSlots = 2;
		_bufferClearRS = std::make_unique<GDX12RootSignature>(resources->Device, RSDesc2);

		// Pipeline State Objects
		D3D12_COMPUTE_PIPELINE_STATE_DESC PSODesc1 = {};
		PSODesc1.pRootSignature = _cullingRS->GetRootSignature().Get();
		PSODesc1.CS = { reinterpret_cast<BYTE*>(_cullingCS->GetBufferPointer()), _cullingCS->GetBufferSize() };

		ThrowIfFailed(resources->Device->GetDevice()->CreateComputePipelineState(
			&PSODesc1, IID_PPV_ARGS(&_cullingPSO)));

		D3D12_COMPUTE_PIPELINE_STATE_DESC PSODesc2 = {};
		PSODesc2.pRootSignature = _bufferClearRS->GetRootSignature().Get();
		PSODesc2.CS = { reinterpret_cast<BYTE*>(_bufferClearCS->GetBufferPointer()), _bufferClearCS->GetBufferSize() };

		ThrowIfFailed(resources->Device->GetDevice()->CreateComputePipelineState(
			&PSODesc2, IID_PPV_ARGS(&_bufferClearPSO)));
	}

	void Execute(PassExecutionContext& context) override
	{
		auto* resources = context.GetDeviceResources();
		auto* cmdList = context.GetCommandList();
		const auto* common = context.GetFrameData();
		const auto& opaque = context.GetOutputDrawStream(OutOpaqueDrawStream);
		const auto& transparent = context.GetOutputDrawStream(OutTransparentDrawStream);
		auto& currentFrameConstants = resources->FrameConstants[context.GetFrameSlotResourcesIndex()];

		cmdList->BeginPixEvent("GPU Mesh Culling", Colors::Blue);

		cmdList->SetComputeRootSignature(_bufferClearRS.get());
		cmdList->SetPipelineState(_bufferClearPSO);
		cmdList->SetDescriptorHeaps({ resources->SRV_UAV_Heap.get() });
		cmdList->SetComputeUAV(0, opaque.CountUAV->GPUHandle);
		cmdList->SetComputeUAV(1, transparent.CountUAV->GPUHandle);
		cmdList->Dispatch(1, 1, 1);
		cmdList->ResourceBarrier({ opaque.Count->GetUAVBarrier(), transparent.Count->GetUAVBarrier() });

		cmdList->SetComputeRootSignature(_cullingRS.get());
		cmdList->SetPipelineState(_cullingPSO);
		cmdList->SetDescriptorHeaps({ resources->SRV_UAV_Heap.get() });
		cmdList->SetComputeRootConstantBufferView(0, currentFrameConstants->CameraCB->GetElementAddress(common->ActiveCameraCBufferIndex));
		cmdList->SetComputeSRV(0, currentFrameConstants->InstanceCache->GetSRV()->GPUHandle);
		cmdList->SetComputeSRV(1, resources->IndirectCommandsCache->GetSRV()->GPUHandle);
		cmdList->SetComputeSRV(2, currentFrameConstants->MaterialCache->GetSRV()->GPUHandle);
		cmdList->SetComputeUAV(0, opaque.ArgumentUAV->GPUHandle);
		cmdList->SetComputeUAV(1, opaque.CountUAV->GPUHandle);
		cmdList->SetComputeUAV(2, transparent.ArgumentUAV->GPUHandle);
		cmdList->SetComputeUAV(3, transparent.CountUAV->GPUHandle);
		const UINT drawCount = resources->IndirectCommandsCache->GetElementCount();
		if (drawCount) { cmdList->Dispatch((drawCount + 63) / 64, 1, 1); }

		cmdList->ResourceBarrier({ opaque.Arguments->GetUAVBarrier(), opaque.Count->GetUAVBarrier(),
			transparent.Arguments->GetUAVBarrier(), transparent.Count->GetUAVBarrier() });
		cmdList->EndPixEvent();
	}

private:
	ComPtr<ID3DBlob> _cullingCS;
	std::unique_ptr<GDX12RootSignature> _cullingRS;
	ComPtr<ID3D12PipelineState> _cullingPSO;

	ComPtr<ID3DBlob> _bufferClearCS;
	std::unique_ptr<GDX12RootSignature> _bufferClearRS;
	ComPtr<ID3D12PipelineState> _bufferClearPSO;
};

#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

class GDX12GPUCullingPass : public GDX12RenderPass
{
public:
	GDX12GPUCullingPass() { _flags = RENDER_PASS_FLAG_USE_CAMERAS; }

	void Initialize(GDX12DeviceResources* resources)
	{
		_resources = resources;

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

	// automatically uses IN_OUT_CameraVisibilityBuffers from cameraCBIndex
	void Execute(GDX12CommandList* cmdList, UINT IN_CameraCBIndex)
	{
		auto& currentFrameConstants = _resources->FrameConstants[_resources->CurrFrameConstantsIndex];
		auto& currentCameraVisBuffers = currentFrameConstants->CameraVisibilityCommands[IN_CameraCBIndex];

		cmdList->BeginPixEvent("GPU Mesh Culling", Colors::Blue);
		cmdList->ResourceBarrier({
			currentCameraVisBuffers.VisibleOpaqueCommandsCache->GetResource().GetUnorderedAccessBarrier(),
			currentCameraVisBuffers.OpaqueDrawCounter->GetResource().GetUnorderedAccessBarrier(),
			currentCameraVisBuffers.VisibleTransparentCommandsCache->GetResource().GetUnorderedAccessBarrier(),
			currentCameraVisBuffers.TransparentDrawCounter->GetResource().GetUnorderedAccessBarrier() });

		cmdList->SetComputeRootSignature(_bufferClearRS.get());
		cmdList->SetPipelineState(_bufferClearPSO);
		cmdList->SetDescriptorHeaps({ _resources->SRV_UAV_Heap.get() });
		cmdList->SetComputeUAV(0, currentCameraVisBuffers.OpaqueDrawCounter->GetUAV()->GPUHandle);
		cmdList->SetComputeUAV(1, currentCameraVisBuffers.TransparentDrawCounter->GetUAV()->GPUHandle);
		cmdList->Dispatch(1, 1, 1);

		cmdList->SetComputeRootSignature(_cullingRS.get());
		cmdList->SetPipelineState(_cullingPSO);
		cmdList->SetDescriptorHeaps({ _resources->SRV_UAV_Heap.get() });
		cmdList->SetComputeRootConstantBufferView(0, currentFrameConstants->CameraCB->GetElementAddress(IN_CameraCBIndex));
		cmdList->SetComputeSRV(0, currentFrameConstants->InstanceCache->GetSRV()->GPUHandle);
		cmdList->SetComputeSRV(1, _resources->IndirectCommandsCache->GetSRV()->GPUHandle);
		cmdList->SetComputeSRV(2, currentFrameConstants->MaterialCache->GetSRV()->GPUHandle);
		cmdList->SetComputeUAV(0, currentCameraVisBuffers.VisibleOpaqueCommandsCache->GetUAV()->GPUHandle);
		cmdList->SetComputeUAV(1, currentCameraVisBuffers.OpaqueDrawCounter->GetUAV()->GPUHandle);
		cmdList->SetComputeUAV(2, currentCameraVisBuffers.VisibleTransparentCommandsCache->GetUAV()->GPUHandle);
		cmdList->SetComputeUAV(3, currentCameraVisBuffers.TransparentDrawCounter->GetUAV()->GPUHandle);
		cmdList->Dispatch((_resources->IndirectCommandsCache->GetElementCount() + 63) / 64, 1, 1);

		cmdList->ResourceBarrier({
			currentCameraVisBuffers.VisibleOpaqueCommandsCache->GetResource().GetUAVBarrier(),
			currentCameraVisBuffers.OpaqueDrawCounter->GetResource().GetUAVBarrier() });
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
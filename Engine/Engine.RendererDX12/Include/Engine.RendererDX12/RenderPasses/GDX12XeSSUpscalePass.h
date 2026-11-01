#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include "Common/GameTimer.h"
#include <stdexcept>

#include "xess/xess.h"
#include "xess/xess_d3d12.h"

class GDX12XeSSUpscalePass : public GDX12RenderPass
{
public:
	GDX12XeSSUpscalePass() : IN_DepthBuffer(nullptr), IN_MotionVectors(nullptr), _XeSSContext(nullptr),
		_XeSSQualityMode(XESS_QUALITY_SETTING_ULTRA_QUALITY_PLUS)
	{ _flags = RENDER_PASS_FLAG_UPSCALER | RENDER_PASS_FLAG_USE_JITTER; }

	~GDX12XeSSUpscalePass() override
	{
		if (_XeSSContext) xessDestroyContext(_XeSSContext);
	}

	virtual void SetInputs(std::vector<IRenderPassLink*> inputs) override
	{
		_inputs = inputs;
		_numInputs = 2;
		MatchOutputCount(static_cast<UINT>(inputs.size() > 2 ? inputs.size() - 2 : 0));
	}

	IRenderPassLink* GetOutput(UINT index) override
	{
		if (_inputs.empty()) { MatchOutputCount(index + 1); }
		return GDX12RenderPass::GetOutput(index);
	}

	// Input 0 - DepthStencil
	// Input 1 - MotionVectors
	// Input N - Input Texture
	// Output N - UpscaledTexture
	void Initialize() override
	{
		_commonData->Upscaler = this;

		IN_DepthBuffer = _inputs[0];
		IN_MotionVectors = _inputs[1];

		GDX12TextureDesc TextureDesc1;
		TextureDesc1.Width = _commonData->WindowWidth;
		TextureDesc1.Height = _commonData->WindowHeight;

		TextureDesc1.CreateSRV = true;
		TextureDesc1.SRV_UAV_Heap = _resources->SRV_UAV_Heap.get();
		TextureDesc1.SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		TextureDesc1.SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		TextureDesc1.SRVDesc.Texture2D.MipLevels = 1;
		TextureDesc1.SRVDesc.Texture2D.MostDetailedMip = 0;
		TextureDesc1.SRVDesc.Texture2D.PlaneSlice = 0;
		TextureDesc1.SRVDesc.Texture2D.ResourceMinLODClamp = 0.0f;

		IN_Textures.resize(_numOutputs);
		for (UINT i = 0; i < _numOutputs; i++)
		{
			IN_Textures[i] = _inputs[i + 2];
			GDX12Texture* inputTexture = IN_Textures[i]->GetTexture();
			TextureDesc1.Format = TextureDesc1.RTVDesc.Format = TextureDesc1.SRVDesc.Format = inputTexture->GetFormat();
			TextureDesc1.SRVHeapIndex = _resources->SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
			OUT_UpscaledTextures[i]->Initialize(TextureDesc1);
		}

	}

	void Setup(GDX12DeviceResources* initOnResources, GDX12DeviceResources* otherResources, RenderPipelineCommonData* commonData) override
	{
		GDX12RenderPass::Setup(initOnResources, otherResources, commonData);

		BuildXeSSContext();
		QueryRenderTargetResolution();
	}

	void Execute(GDX12CommandList* cmdList) override
	{
		GDX12Texture* depthStencil = IN_DepthBuffer->GetTexture();
		GDX12Texture* motionVectors = IN_MotionVectors->GetTexture();

		cmdList->BeginPixEvent("XeSS Upscale Pass", Colors::Blue);

		cmdList->ResourceBarrier({ depthStencil->GetResource()->GetNonPixelShaderResourceBarrier(),
			motionVectors->GetResource()->GetNonPixelShaderResourceBarrier() });

		xess_d3d12_execute_params_t execParams = {};
		execParams.pDepthTexture = depthStencil->GetResource()->D3DResource.Get();
		execParams.pVelocityTexture = motionVectors->GetResource()->D3DResource.Get();
		initParams.initFlags = XESS_INIT_FLAG_INVERTED_DEPTH | 

#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include "Common/GameTimer.h"
#include "Engine.RendererDX12/RenderSettings.h"
#include <stdexcept>

#include "xess/xess.h"
#include "xess/xess_d3d12.h"

class GDX12XeSSUpscalePass : public GDX12RenderPass
{
public:
	GDX12XeSSUpscalePass() : IN_DepthBuffer(nullptr), IN_MotionVectors(nullptr), _XeSSContext(nullptr),
		_XeSSQualityMode(XESS_QUALITY_SETTING_ULTRA_QUALITY_PLUS)
	{ _flags = RENDER_PASS_FLAG_UPSCALER; }

    ~GDX12XeSSUpscalePass() override
    {
        if (_XeSSContext)
        {
            _resources->Device->GetCommandQueue()->Flush();
            xessDestroyContext(_XeSSContext);
        }
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
        TextureDesc1.CreateUAV = true;
        TextureDesc1.UAVDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
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
            if (inputTexture->GetFormat() != DXGI_FORMAT_R16G16B16A16_FLOAT)
                throw std::invalid_argument("XeSS requires linear HDR RGBA16F input");
            TextureDesc1.Format = TextureDesc1.RTVDesc.Format = TextureDesc1.SRVDesc.Format = TextureDesc1.UAVDesc.Format = inputTexture->GetFormat();
            TextureDesc1.UAVHeapIndex = _resources->SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
			TextureDesc1.SRVHeapIndex = _resources->SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
			OUT_UpscaledTextures[i]->Initialize(TextureDesc1);
		}

	}

	void Setup(GDX12DeviceResources* initOnResources, GDX12DeviceResources* otherResources, RenderPipelineCommonData* commonData) override
	{
		GDX12RenderPass::Setup(initOnResources, otherResources, commonData);

		ApplySettings();
		if (_settings.Enabled) BuildXeSSContext();
		QueryRenderTargetResolution();
	}

    bool ApplySettings()
    {
        const auto next = RenderSettings::GetUpscaler();
        const bool changed = next.Enabled != _settings.Enabled || (next.Enabled && next.Level != _settings.Level);
        _settings = next;
        switch (next.Level)
        {
        case RenderSettings::Quality::Low: _XeSSQualityMode = XESS_QUALITY_SETTING_PERFORMANCE; break;
        case RenderSettings::Quality::Mid: _XeSSQualityMode = XESS_QUALITY_SETTING_BALANCED; break;
        default: _XeSSQualityMode = XESS_QUALITY_SETTING_ULTRA_QUALITY_PLUS; break;
        }
        SetFlag(RENDER_PASS_FLAG_USE_JITTER, next.Enabled);
        return changed;
    }

	void Execute(GDX12CommandList* cmdList) override
	{
		if (!_settings.Enabled)
		{
			cmdList->BeginPixEvent("Native HDR copy", Colors::Blue);
			for (UINT i = 0; i < _numOutputs; ++i)
			{
				auto* input = IN_Textures[i]->GetTexture()->GetResource();
				auto* output = OUT_UpscaledTextures[i]->GetResource();
				cmdList->ResourceBarrier({ input->GetCopySourceBarrier(), output->GetCopyDestBarrier() });
				cmdList->CopyResource(output->D3DResource.Get(), input->D3DResource.Get());
				cmdList->ResourceBarrier({ input->GetSRVBarrier(), output->GetSRVBarrier() });
			}
			cmdList->EndPixEvent();
			return;
		}
		GDX12Texture* depthStencil = IN_DepthBuffer->GetTexture();
		GDX12Texture* motionVectors = IN_MotionVectors->GetTexture();

		cmdList->BeginPixEvent("XeSS Upscale Pass", Colors::Blue);

		cmdList->ResourceBarrier({ depthStencil->GetResource()->GetNonPixelShaderResourceBarrier(),
			motionVectors->GetResource()->GetNonPixelShaderResourceBarrier() });

		xess_d3d12_execute_params_t execParams = {};
		execParams.pDepthTexture = depthStencil->GetResource()->D3DResource.Get();
		execParams.pVelocityTexture = motionVectors->GetResource()->D3DResource.Get();
		// RH projection shear displaces geometry by -jitter (Intel XeSS developer guide, Jitter).
		execParams.jitterOffsetX = -_commonData->ActiveCameraJitterOffsetX;
		execParams.jitterOffsetY = -_commonData->ActiveCameraJitterOffsetY;
		execParams.inputWidth = _commonData->DownscaledWidth;
		execParams.inputHeight = _commonData->DownscaledHeight;
		execParams.resetHistory = _historyRevision != _commonData->HistoryRevision;

		execParams.exposureScale = 1;
		execParams.inputColorBase = { 0,0 };
		execParams.inputDepthBase = { 0,0 };

		for (UINT i = 0; i < _numOutputs; i++)
		{
			GDX12Texture* inputTexture = IN_Textures[i]->GetTexture();
			GDX12Texture* upscaledTexture = OUT_UpscaledTextures[i].get();

			execParams.pColorTexture = inputTexture->GetResource()->D3DResource.Get();
			execParams.pOutputTexture = upscaledTexture->GetResource()->D3DResource.Get();
			
			cmdList->ResourceBarrier({ inputTexture->GetResource()->GetNonPixelShaderResourceBarrier(),
			upscaledTexture->GetResource()->GetUnorderedAccessBarrier() });

			xess_result_t execResult = xessD3D12Execute(_XeSSContext, cmdList->GetCommandList().Get(), &execParams);
            cmdList->InvalidateCachedState();
            if (execResult != XESS_RESULT_SUCCESS)
                throw std::runtime_error("XeSS execute failed: " + XeSSResultToString(execResult));
            cmdList->ResourceBarrier({ upscaledTexture->GetResource()->GetSRVBarrier() });
		}
		_historyRevision = _commonData->HistoryRevision;
        cmdList->EndPixEvent();
	}

	void Resize() override
	{
		for (auto& texture : OUT_UpscaledTextures) { texture->Resize(_commonData->WindowWidth, _commonData->WindowHeight); }
        if (_XeSSContext) xessDestroyContext(_XeSSContext);
        _XeSSContext = nullptr;
        if (_settings.Enabled) BuildXeSSContext();
        _historyRevision = UINT64_MAX;
	}

	void QueryRenderTargetResolution() override
	{
		if (!_settings.Enabled)
		{
			_commonData->DownscaledWidth = _commonData->WindowWidth;
			_commonData->DownscaledHeight = _commonData->WindowHeight;
			return;
		}
		xess_2d_t targetResolution = { _commonData->WindowWidth, _commonData->WindowHeight } ;
		xess_2d_t downscaledResolution;
		xess_2d_t minResolution;
		xess_2d_t maxResolution;
		xess_result_t queryResolutionResult = xessGetOptimalInputResolution(_XeSSContext, 
			&targetResolution, _XeSSQualityMode, &downscaledResolution, &minResolution, &maxResolution);
		if (queryResolutionResult != XESS_RESULT_SUCCESS)
		{
            throw std::runtime_error("XeSS query resolution failed: " + XeSSResultToString(queryResolutionResult));
		}
		_commonData->DownscaledWidth = downscaledResolution.x;
		_commonData->DownscaledHeight = downscaledResolution.y;
	}

	void ClearDenendencies() override
	{
		GDX12RenderPass::ClearDenendencies();
        if (_XeSSContext)
        {
            _resources->Device->GetCommandQueue()->Flush();
            xessDestroyContext(_XeSSContext);
            _XeSSContext = nullptr;
        }
		IN_DepthBuffer = nullptr;
		IN_MotionVectors = nullptr;
		IN_Textures.clear();
		OUT_UpscaledTextures.clear();
	}

private:
	IRenderPassLink* IN_DepthBuffer;
	IRenderPassLink* IN_MotionVectors;
	std::vector<IRenderPassLink*> IN_Textures;
	std::vector<std::unique_ptr<GDX12Texture>> OUT_UpscaledTextures;
	xess_context_handle_t _XeSSContext;
	xess_quality_settings_t _XeSSQualityMode;
    RenderSettings::Upscaler _settings = { false, RenderSettings::Quality::High };
    uint64_t _historyRevision = UINT64_MAX;

	void MatchOutputCount(UINT outputCount)
	{
		if (OUT_UpscaledTextures.size() < outputCount) { OUT_UpscaledTextures.resize(outputCount); }
		if (_outputs.size() < outputCount) { _outputs.resize(outputCount, nullptr); }

		for (UINT i = 0; i < outputCount; i++)
		{
			if (!OUT_UpscaledTextures[i]) { OUT_UpscaledTextures[i] = std::make_unique<GDX12Texture>(); }
			_outputs[i] = OUT_UpscaledTextures[i].get();
		}

		_numOutputs = outputCount;
	}

	void BuildXeSSContext()
	{
		xess_result_t createContextResult = xessD3D12CreateContext(_resources->Device->GetDevice().Get(), &_XeSSContext);
		if (createContextResult != XESS_RESULT_SUCCESS)
		{
            throw std::runtime_error("XeSS context not created: " + XeSSResultToString(createContextResult));
		}

		xess_d3d12_init_params_t initParams = {};
		initParams.outputResolution.x = _commonData->WindowWidth;
		initParams.outputResolution.y = _commonData->WindowHeight;
        initParams.initFlags = XESS_INIT_FLAG_INVERTED_DEPTH | XESS_INIT_FLAG_USE_NDC_VELOCITY;
		initParams.qualitySetting = _XeSSQualityMode;

		xess_result_t initResult = xessD3D12Init(_XeSSContext, &initParams);
		if (initResult != XESS_RESULT_SUCCESS) 
		{
            xessDestroyContext(_XeSSContext);
            _XeSSContext = nullptr;
            throw std::runtime_error("XeSS context not initialized: " + XeSSResultToString(initResult));
		}
	}

	std::string XeSSResultToString(const xess_result_t result)
	{
		switch (result)
		{
		case XESS_RESULT_SUCCESS: return "XESS_RESULT_SUCCESS";
		case XESS_RESULT_WARNING_NONEXISTING_FOLDER: return "XESS_RESULT_WARNING_NONEXISTING_FOLDER";
		case XESS_RESULT_WARNING_OLD_DRIVER: return "XESS_RESULT_WARNING_OLD_DRIVER";
		case XESS_RESULT_ERROR_UNSUPPORTED_DEVICE: return "XESS_RESULT_ERROR_UNSUPPORTED_DEVICE";
		case XESS_RESULT_ERROR_UNSUPPORTED_DRIVER: return "XESS_RESULT_ERROR_UNSUPPORTED_DRIVER";
		case XESS_RESULT_ERROR_UNINITIALIZED: return "XESS_RESULT_ERROR_UNINITIALIZED";
		case XESS_RESULT_ERROR_INVALID_ARGUMENT: return "XESS_RESULT_ERROR_INVALID_ARGUMENT";
		case XESS_RESULT_ERROR_DEVICE_OUT_OF_MEMORY: return "XESS_RESULT_ERROR_DEVICE_OUT_OF_MEMORY";
		case XESS_RESULT_ERROR_DEVICE: return "XESS_RESULT_ERROR_DEVICE";
		case XESS_RESULT_ERROR_NOT_IMPLEMENTED: return "XESS_RESULT_ERROR_NOT_IMPLEMENTED";
		case XESS_RESULT_ERROR_INVALID_CONTEXT: return "XESS_RESULT_ERROR_INVALID_CONTEXT";
		case XESS_RESULT_ERROR_OPERATION_IN_PROGRESS: return "XESS_RESULT_ERROR_OPERATION_IN_PROGRESS";
		case XESS_RESULT_ERROR_UNSUPPORTED: return "XESS_RESULT_ERROR_UNSUPPORTED";
		case XESS_RESULT_ERROR_CANT_LOAD_LIBRARY: return "XESS_RESULT_ERROR_CANT_LOAD_LIBRARY";
		case XESS_RESULT_ERROR_WRONG_CALL_ORDER: return "XESS_RESULT_ERROR_WRONG_CALL_ORDER";
		case XESS_RESULT_ERROR_UNKNOWN: return "XESS_RESULT_ERROR_UNKNOWN";
		default: return "XESS_RESULT_UNKNOWN_ENUM";
		}
	}
};

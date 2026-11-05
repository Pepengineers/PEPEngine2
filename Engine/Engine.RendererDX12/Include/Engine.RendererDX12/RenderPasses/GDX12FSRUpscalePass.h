#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include "Common/GameTimer.h"
#include <stdexcept>

#include "ffx-api/ffx_api.h"
#include "ffx-api/ffx_upscale.h"
#include "ffx-api/dx12/ffx_api_dx12.h"

class GDX12FSRUpscalePass : public GDX12RenderPass
{
public:
	explicit GDX12FSRUpscalePass(FfxApiUpscaleQualityMode quality = FFX_UPSCALE_QUALITY_MODE_QUALITY) : IN_DepthBuffer(nullptr), IN_MotionVectors(nullptr), _FFXContext(nullptr),
		_FSRQualityMode(quality)
	{ _numInputs = 2; _flags = RENDER_PASS_FLAG_UPSCALER | RENDER_PASS_FLAG_USE_JITTER; }

	~GDX12FSRUpscalePass() override { DestroyContext(); }

	virtual void SetInputs(std::vector<IRenderPassLink*> inputs) override
	{
		GDX12RenderPass::SetInputs(std::move(inputs));
		MatchOutputCount(static_cast<UINT>(_inputs.size() > 2 ? _inputs.size() - 2 : 0));
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

        IN_Textures.resize(_numOutputs);
        for (UINT i = 0; i < _numOutputs; ++i)
        {
            IN_Textures[i] = _inputs[i + 2];
            GDX12RenderHelpers::InitializeComputeTexture(*OUT_UpscaledTextures[i], *_resources,
                _commonData->WindowWidth, _commonData->WindowHeight, IN_Textures[i]->GetTexture()->GetFormat());
        }

	}

	void Setup(GDX12DeviceResources* initOnResources, GDX12DeviceResources* otherResources, RenderPipelineCommonData* commonData) override
	{
		GDX12RenderPass::Setup(initOnResources, otherResources, commonData);

		BuildFSRContext();
		QueryRenderTargetResolution();
	}

	void Execute(GDX12CommandList* cmdList) override
	{
		GDX12Texture* depthStencil = IN_DepthBuffer->GetTexture();
		GDX12Texture* motionVectors = IN_MotionVectors->GetTexture();

		GDX12_SCOPED_EVENT(cmdList, "FSR Upscale Pass", Colors::Blue);
		ffxDispatchDescUpscale dispatchDesc = {};
		dispatchDesc.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
		dispatchDesc.commandList = cmdList->GetCommandList().Get();
		dispatchDesc.depth = ffxApiGetResourceDX12(depthStencil->GetResource()->D3DResource.Get(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
		dispatchDesc.motionVectors = ffxApiGetResourceDX12(motionVectors->GetResource()->D3DResource.Get(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
		// Resolution before upscaling
		dispatchDesc.renderSize = { _commonData->DownscaledWidth, _commonData->DownscaledHeight };
		dispatchDesc.motionVectorScale = { static_cast<float>(_commonData->DownscaledWidth) * 0.5f,
			-static_cast<float>(_commonData->DownscaledHeight) * 0.5f };
		dispatchDesc.upscaleSize = { _commonData->WindowWidth, _commonData->WindowHeight };
		// We use inverted depth, this is not a mistake
		dispatchDesc.cameraNear = _commonData->ActiveCameraFarPlane;
		dispatchDesc.cameraFar = _commonData->ActiveCameraNearPlane;
		dispatchDesc.cameraFovAngleVertical = XMConvertToRadians(_commonData->ActiveCameraFOV);

        // FSR subtracts jitter from source pixel centers; the engine stores positive raster displacement.
		dispatchDesc.jitterOffset.x = _commonData->ActiveCameraJitterOffsetX;
		dispatchDesc.jitterOffset.y = _commonData->ActiveCameraJitterOffsetY;

		dispatchDesc.enableSharpening = true;
		dispatchDesc.sharpness = 1.f;
		// for whatever reason, it doesnt like it when frame time is lower than 1.f
		float clampedTime = (std::max)(_commonData->GameTimer->DeltaTime() * 1000.f, 1.f);
		dispatchDesc.frameTimeDelta = clampedTime; //expects milliseconds
		dispatchDesc.reset = _commonData->ResetUpscalerHistory || _commonData->ResetHistory || _historyRevision != _commonData->HistoryRevision;
		
		dispatchDesc.preExposure = 1.f;
		dispatchDesc.viewSpaceToMetersFactor = 0.01f;

		dispatchDesc.exposure = ffxApiGetResourceDX12(nullptr, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
		dispatchDesc.reactive = ffxApiGetResourceDX12(nullptr, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
		dispatchDesc.transparencyAndComposition = ffxApiGetResourceDX12(nullptr, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
		//dispatchDesc.flags = FFX_UPSCALE_FLAG_DRAW_DEBUG_VIEW;
		dispatchDesc.flags = 0;

        // FFX input descriptors declare shader-read state without read-only DSV bits.
        if (depthStencil->GetResource()->GetCurrentState() & D3D12_RESOURCE_STATE_DEPTH_READ)
            cmdList->ResourceBarrier({ depthStencil->GetResource()->GetCommonBarrier() });
		for (UINT i = 0; i < _numOutputs; i++)
		{
			GDX12Texture* inputTexture = IN_Textures[i]->GetTexture();
			GDX12Texture* upscaledTexture = OUT_UpscaledTextures[i].get();
			dispatchDesc.color = ffxApiGetResourceDX12(inputTexture->GetResource()->D3DResource.Get(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
			dispatchDesc.output = ffxApiGetResourceDX12(upscaledTexture->GetResource()->D3DResource.Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);

			cmdList->ResourceBarrier({ depthStencil->GetResource()->GetSRVBarrier(), motionVectors->GetResource()->GetSRVBarrier(),
                inputTexture->GetResource()->GetSRVBarrier(), upscaledTexture->GetResource()->GetUnorderedAccessBarrier() });
            ffxReturnCode_t dispatchError = ffxDispatch(&_FFXContext, &dispatchDesc.header);
            cmdList->InvalidateCachedState();
			if (dispatchError != FFX_API_RETURN_OK)
			{
				std::string errorMsg = "FSR DISPATCH ERROR: " + std::to_string(dispatchError) + " \n";
				throw std::runtime_error(errorMsg);
			}
            cmdList->ResourceBarrier({ upscaledTexture->GetResource()->GetUAVBarrier() });
        }
        _commonData->ResetUpscalerHistory = false;
        _commonData->ResetHistory = false;
        _historyRevision = _commonData->HistoryRevision;
	}

	void Resize() override
	{
		for (auto& texture : OUT_UpscaledTextures) { texture->Resize(_commonData->WindowWidth, _commonData->WindowHeight); }
		DestroyContext();
		BuildFSRContext();
	}

	void QueryRenderTargetResolution() override
	{
		ffxQueryDescUpscaleGetRenderResolutionFromQualityMode queryDesc = {};
		queryDesc.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETRENDERRESOLUTIONFROMQUALITYMODE;
		queryDesc.displayHeight = _commonData->WindowHeight;
		queryDesc.displayWidth = _commonData->WindowWidth;
		queryDesc.qualityMode = _FSRQualityMode;
		queryDesc.pOutRenderHeight = &_commonData->DownscaledHeight;
		queryDesc.pOutRenderWidth = &_commonData->DownscaledWidth;
		if (ffxQuery(&_FFXContext, &queryDesc.header) != FFX_API_RETURN_OK)
            throw std::runtime_error("FSR render-resolution query failed");
	}

	void ClearDenendencies() override
	{
		GDX12RenderPass::ClearDenendencies();

		DestroyContext();
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
	ffxContext _FFXContext;
	FfxApiUpscaleQualityMode _FSRQualityMode;
    uint64_t _historyRevision = UINT64_MAX;
    void DestroyContext()
    {
        if (!_FFXContext) return;
        _resources->Device->GetCommandQueue()->Flush();
        ffxDestroyContext(&_FFXContext, nullptr);
        _FFXContext = nullptr;
    }

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

	void BuildFSRContext()
	{
		ffxCreateBackendDX12Desc backendDesc = {};
		backendDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
		backendDesc.device = _resources->Device->GetDevice().Get();

        ffxCreateContextDescUpscaleVersion version = {};
        version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
        version.header.pNext = &backendDesc.header;
        version.version = FFX_UPSCALER_VERSION;

		ffxCreateContextDescUpscale upscaleDesc = {};
		upscaleDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
		upscaleDesc.header.pNext = &version.header;
		upscaleDesc.maxRenderSize = { static_cast<uint32_t>(_commonData->WindowWidth), static_cast<uint32_t>(_commonData->WindowHeight) };
		upscaleDesc.maxUpscaleSize = { static_cast<uint32_t>(_commonData->WindowWidth), static_cast<uint32_t>(_commonData->WindowHeight) };
		upscaleDesc.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_DEPTH_INVERTED | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
		upscaleDesc.fpMessage = [](uint32_t type, const wchar_t* message)
			{
				std::wstring prefix;
				switch (type) {
				case FFX_API_MESSAGE_TYPE_ERROR:
					prefix = L"[FFX ERROR] ";
					break;
				case FFX_API_MESSAGE_TYPE_WARNING:
					prefix = L"[FFX WARNING] ";
					break;
				default:
					prefix = L"[FFX DEBUG] ";
					break;
				}

				std::wstring fullMessage = prefix + message + L"\n";
				OutputDebugStringW(fullMessage.c_str());
			};

		ffxReturnCode_t errorCode = ffxCreateContext(&_FFXContext, &upscaleDesc.header, nullptr);
		if (errorCode != FFX_API_RETURN_OK)
		{
			std::string errorMsg = "ERROR: FSR3 CONTEXT CREATION FAILED\n";
			throw std::runtime_error(errorMsg);
		}
        _commonData->ResetUpscalerHistory = true;
	}
};

#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include "Common/GameTimer.h"

#include "ffx-api/ffx_api.h"
#include "ffx-api/ffx_upscale.h"
#include "ffx-api/dx12/ffx_api_dx12.h"

class GDX12FSRUpscalePass : public GDX12RenderPass
{
public:
	GDX12FSRUpscalePass() : IN_DepthBuffer(nullptr), IN_MotionVectors(nullptr), _FFXContext(nullptr),
		_FSRQualityMode(FFX_UPSCALE_QUALITY_MODE_QUALITY)
	{ _flags = RENDER_PASS_FLAG_UPSCALER | RENDER_PASS_FLAG_USE_JITTER; }

	virtual void SetInputs(std::vector<IRenderPassLink*> inputs) override
	{
		_inputs = inputs;
		_numInputs = 2;
		MatchOutputCount(inputs.size() > 2 ? inputs.size() - 2 : 0);
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

		BuildFSRContext();
		QueryRenderTargetResolution();
	}

	void Execute(GDX12CommandList* cmdList) override
	{
		GDX12Texture* depthStencil = IN_DepthBuffer->GetTexture();
		GDX12Texture* motionVectors = IN_MotionVectors->GetTexture();

		cmdList->BeginPixEvent("FSR Upscale Pass", Colors::Blue);
		ffxDispatchDescUpscale dispatchDesc;
		dispatchDesc.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
		dispatchDesc.commandList = cmdList->GetCommandList().Get();
		dispatchDesc.depth = ffxApiGetResourceDX12(depthStencil->GetResource()->D3DResource.Get(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
		dispatchDesc.motionVectors = ffxApiGetResourceDX12(motionVectors->GetResource()->D3DResource.Get(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
		// Resolution before upscaling
		dispatchDesc.renderSize = { _commonData->DownscaledWidth, _commonData->DownscaledHeight };
		dispatchDesc.motionVectorScale = { static_cast<float>(_commonData->DownscaledWidth), 
			static_cast<float>(_commonData->DownscaledHeight) };
		dispatchDesc.upscaleSize = { _commonData->WindowWidth, _commonData->WindowHeight };
		// We use inverted depth, this is not a mistake
		dispatchDesc.cameraNear = _commonData->ActiveCameraFarPlane;
		dispatchDesc.cameraFar = _commonData->ActiveCameraNearPlane;
		dispatchDesc.cameraFovAngleVertical = XMConvertToRadians(_commonData->ActiveCameraFOV);

		dispatchDesc.jitterOffset.x = -_commonData->ActiveCameraJitterOffsetX;
		dispatchDesc.jitterOffset.y = -_commonData->ActiveCameraJitterOffsetY;

		dispatchDesc.enableSharpening = true;
		dispatchDesc.sharpness = 1.f;
		// for whatever reason, it doesnt like it when frame time is lower than 1.f
		float clampedTime = std::max(_commonData->GameTimer->DeltaTime() * 1000.f, 1.f);
		dispatchDesc.frameTimeDelta = clampedTime; //expects milliseconds
		dispatchDesc.reset = false; // set to true if camera teleports or moves not smoothly
		
		dispatchDesc.preExposure = 1.f;
		dispatchDesc.viewSpaceToMetersFactor = 1.f;

		dispatchDesc.exposure = ffxApiGetResourceDX12(nullptr, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
		dispatchDesc.reactive = ffxApiGetResourceDX12(nullptr, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
		dispatchDesc.transparencyAndComposition = ffxApiGetResourceDX12(nullptr, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
		//dispatchDesc.flags = FFX_UPSCALE_FLAG_DRAW_DEBUG_VIEW;
		dispatchDesc.flags = 0;
		for (int i = 0; i < _numOutputs; i++)
		{
			GDX12Texture* inputTexture = IN_Textures[i]->GetTexture();
			GDX12Texture* upscaledTexture = OUT_UpscaledTextures[i].get();
			dispatchDesc.color = ffxApiGetResourceDX12(inputTexture->GetResource()->D3DResource.Get(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
			dispatchDesc.output = ffxApiGetResourceDX12(upscaledTexture->GetResource()->D3DResource.Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);

			ffxReturnCode_t dispatchError = ffxDispatch(&_FFXContext, &dispatchDesc.header);
			if (dispatchError != FFX_API_RETURN_OK)
			{
				std::string errorMsg = "FSR DISPATCH ERROR: " + std::to_string(dispatchError) + " \n";
				OutputDebugStringA(errorMsg.c_str());
			}
		}
		cmdList->EndPixEvent();
	}

	void Resize() override
	{
		for (auto& texture : OUT_UpscaledTextures) { texture->Resize(_commonData->WindowWidth, _commonData->WindowHeight); }
		ffxDestroyContext(&_FFXContext, nullptr);
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
		ffxQuery(&_FFXContext, &queryDesc.header);
	}

	void ClearDenendencies() override
	{
		GDX12RenderPass::ClearDenendencies();

		if (_FFXContext) { ffxDestroyContext(&_FFXContext, nullptr); }
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

		ffxCreateContextDescUpscale upscaleDesc = {};
		upscaleDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
		upscaleDesc.header.pNext = &backendDesc.header;
		upscaleDesc.maxRenderSize = { static_cast<uint32_t>(_commonData->WindowWidth), static_cast<uint32_t>(_commonData->WindowHeight) };
		upscaleDesc.maxUpscaleSize = { static_cast<uint32_t>(_commonData->WindowWidth), static_cast<uint32_t>(_commonData->WindowHeight) };
		upscaleDesc.flags = FFX_UPSCALE_ENABLE_DEBUG_CHECKING | FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
		upscaleDesc.fpMessage = [](uint32_t type, const wchar_t* message)
			{
				std::wstring wideMessage(message);
				std::string narrowMessage(wideMessage.begin(), wideMessage.end());

				std::string prefix;
				switch (type) {
				case FFX_API_MESSAGE_TYPE_ERROR:
					prefix = "[FFX ERROR] ";
					break;
				case FFX_API_MESSAGE_TYPE_WARNING:
					prefix = "[FFX WARNING] ";
					break;
				default:
					prefix = "[FFX DEBUG] ";
					break;
				}

				std::string fullMessage = prefix + narrowMessage + "\n";
				OutputDebugStringA(fullMessage.c_str());
			};

		ffxReturnCode_t errorCode = ffxCreateContext(&_FFXContext, &upscaleDesc.header, nullptr);
		if (errorCode != FFX_API_RETURN_OK)
		{
			std::string errorMsg = "ERROR: FSR3 CONTEXT CREATION FAILED\n";
			OutputDebugStringA(errorMsg.c_str());
		}
	}
};

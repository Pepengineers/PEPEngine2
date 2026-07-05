#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include "Common/GameTimer.h"

// HOLY SHIT
// we need this cray include since SL's free() function conflits with CRT library's free() function
#ifdef free
#pragma push_macro("free")
#undef free
#define PEP_RESTORE_FREE_MACRO_DLSS_PASS 1
#endif
#include "nvidia-sdk/sl.h"
#include "nvidia-sdk/sl_consts.h"
#include "nvidia-sdk/sl_dlss.h"
#ifdef PEP_RESTORE_FREE_MACRO_DLSS_PASS
#pragma pop_macro("free")
#undef PEP_RESTORE_FREE_MACRO_DLSS_PASS
#endif

class GDX12DLSSUpscalePass : public GDX12RenderPass
{
public:
	GDX12DLSSUpscalePass() : IN_DepthBuffer(nullptr), IN_MotionVectors(nullptr), _viewportHandle(1337),
		_DLSSQualityMode(sl::DLSSMode::eMaxQuality)
	{ _flags = RENDER_PASS_FLAG_UPSCALER | RENDER_PASS_FLAG_USE_JITTER; }

	virtual void SetInputs(std::vector<IRenderPassLink*> inputs) override
	{
		_inputs = inputs;
		_numInputs = 2;
		_numOutputs = inputs.size() - 2;

		OUT_UpscaledTextures.clear();
		_outputs.clear();
		OUT_UpscaledTextures.resize(_numOutputs);
		_outputs.resize(_numOutputs);
		for (int i = 0; i < _numOutputs; i++)
		{
			OUT_UpscaledTextures[i] = std::make_unique<GDX12Texture>();
			_outputs[i] = OUT_UpscaledTextures[i].get();
		}
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

		IN_Textures.resize(_numInputs);
		for (int i = 0; i < _numOutputs; i++)
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

		InitSreamline();
		CreateDLSSFeature();
		QueryRenderTargetResolution();
	}

	void Execute(GDX12CommandList* cmdList) override
	{
		using namespace sl;

		cmdList->BeginPixEvent("DLSS Upscale Pass", Colors::Blue);

		FrameToken* currentFrame = nullptr;
		Result result = slGetNewFrameToken(currentFrame);
		SetFrameConstants(currentFrame);

		GDX12Texture* depthStencil = IN_DepthBuffer->GetTexture();
		GDX12Texture* motionVectors = IN_MotionVectors->GetTexture();

		cmdList->ResourceBarrier({ depthStencil->GetResource()->GetDepthReadBarrier(),
			motionVectors->GetResource()->GetPixelShaderResourceBarrier() });

		Resource depthResource = Resource{ ResourceType::eTex2d,
		depthStencil->GetResource()->D3DResource.Get(),
		nullptr, nullptr, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_DEPTH_READ };

		Resource mvecResource = Resource{ ResourceType::eTex2d,
			motionVectors->GetResource()->D3DResource.Get(),
			nullptr, nullptr, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE };

		Extent fullExtent{};
		fullExtent.top = 0;
		fullExtent.left = 0;
		fullExtent.width = _commonData->DownscaledWidth;
		fullExtent.height = _commonData->DownscaledHeight;

		ResourceTag depthTag{ &depthResource, kBufferTypeDepth, ResourceLifecycle::eValidUntilPresent, &fullExtent };
		ResourceTag mvecTag{ &mvecResource, kBufferTypeMotionVectors, ResourceLifecycle::eValidUntilPresent, &fullExtent };

		for (int i = 0; i < _numOutputs; i++)
		{
			GDX12Texture* inputTexture = IN_Textures[i]->GetTexture();
			GDX12Texture* upscaledTexture = OUT_UpscaledTextures[i].get();

			cmdList->ResourceBarrier({ inputTexture->GetResource()->GetPixelShaderResourceBarrier(),
				upscaledTexture->GetResource()->GetUnorderedAccessBarrier() });

			Resource inputResource = Resource{ ResourceType::eTex2d,
				inputTexture->GetResource()->D3DResource.Get(),
				nullptr, nullptr, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE };

			Resource outputResource = Resource{ ResourceType::eTex2d,
				upscaledTexture->GetResource()->D3DResource.Get(),
				nullptr, nullptr, D3D12_RESOURCE_STATE_UNORDERED_ACCESS };

			ResourceTag inputTag{ &inputResource, kBufferTypeScalingInputColor, ResourceLifecycle::eValidUntilPresent, nullptr };
			ResourceTag outputTag{ &outputResource, kBufferTypeScalingOutputColor, ResourceLifecycle::eValidUntilPresent, nullptr };

			ResourceTag tags[] = { depthTag, mvecTag, inputTag, outputTag };
			result = slSetTagForFrame(*currentFrame, _viewportHandle, tags, _countof(tags), cmdList->GetCommandList().Get());
			if (result != Result::eOk)
			{
				std::string errorMsg = "DLSS: Failed to set tags: " + SLResultToString(result) + "\n";
				OutputDebugStringA(errorMsg.c_str());
			}

			const BaseStructure* inputs[] = { &_viewportHandle };
			result = slEvaluateFeature(kFeatureDLSS, *currentFrame, inputs, _countof(inputs), cmdList->GetCommandList().Get());
			if (result != Result::eOk)
			{
				std::string errorMsg = "DLSS EVALUATE ERROR: " + SLResultToString(result) + "\n";
				OutputDebugStringA(errorMsg.c_str());
			}

		}
		cmdList->EndPixEvent();
	}

	void Resize() override
	{
		for (auto& texture : OUT_UpscaledTextures) { texture->Resize(_commonData->WindowWidth, _commonData->WindowHeight); }
		slFreeResources(sl::kFeatureDLSS, _viewportHandle);
		slDLSSSetOptions(_viewportHandle, sl::DLSSOptions{});
		CreateDLSSFeature();
	}

	void QueryRenderTargetResolution() override
	{
		using namespace sl;

		DLSSOptions dlssOptions = {};
		dlssOptions.mode = _DLSSQualityMode;
		dlssOptions.outputWidth = _commonData->WindowWidth;
		dlssOptions.outputHeight = _commonData->WindowHeight;

		DLSSOptimalSettings optimalSettings = {};
		Result result = slDLSSGetOptimalSettings(dlssOptions, optimalSettings);
		if (result != Result::eOk)
		{
			std::string errorMsg = "DLSS: Failed to get optimal settings: " + SLResultToString(result) + "\n";
			OutputDebugStringA(errorMsg.c_str());
		}

		_commonData->DownscaledWidth = optimalSettings.optimalRenderWidth;
		_commonData->DownscaledHeight = optimalSettings.optimalRenderHeight;
	}

	void ClearDenendencies() override
	{
		GDX12RenderPass::ClearDenendencies();
		slFreeResources(sl::kFeatureDLSS, _viewportHandle);
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
	sl::ViewportHandle _viewportHandle;
	sl::DLSSMode _DLSSQualityMode;

	void InitSreamline()
	{
		using namespace sl;
		// Init streamline
		Preferences preferences{};
		preferences.showConsole = false;
		preferences.logLevel = LogLevel::eVerbose;
		preferences.engine = EngineType::eCustom;
		preferences.renderAPI = RenderAPI::eD3D12;
		Feature myFeatures[] = { kFeatureDLSS };
		preferences.featuresToLoad = myFeatures;
		preferences.numFeaturesToLoad = _countof(myFeatures);
		preferences.flags = PreferenceFlags::eUseFrameBasedResourceTagging;

		preferences.logMessageCallback = [](LogType type, const char* msg)
			{
				std::string prefix;
				switch (type) {
				case LogType::eError: prefix = "SL ERROR: "; break;
				case LogType::eWarn: prefix = "SL WARNING: "; break;
				default: prefix = "SL INFO: "; break;
				}
				OutputDebugStringA((prefix + msg + "\n").c_str());
			};
		
		Result result = slInit(preferences, sl::kSDKVersion);
		if (result != sl::Result::eOk)
		{
			std::string msg = "ERROR: Streamline initialization failed: " + SLResultToString(result) + "\n";
			OutputDebugStringA(msg.c_str());
		}

		// Set Device
		result = slSetD3DDevice(_resources->Device->GetDevice().Get());
		if (result != Result::eOk)
		{
			std::string errorMsg = "ERROR: Failed to set D3D12 device: " + SLResultToString(result) + "\n";
			OutputDebugStringA(errorMsg.c_str());
		}

		// Check DLSS support on device
		AdapterInfo adapterInfo = {};
		_resources->Device->GetDevice()->GetAdapterLuid();
		LUID luid = _resources->Device->GetDevice()->GetAdapterLuid();
		adapterInfo.deviceLUID = (uint8_t*)&luid;
		adapterInfo.deviceLUIDSizeInBytes = sizeof(LUID);

		result = slIsFeatureSupported(kFeatureDLSS, adapterInfo);
		if (result != Result::eOk)
		{
			std::string errorMsg = "ERROR: DLSS is not supported: " + SLResultToString(result) + "\n";
			OutputDebugStringA(errorMsg.c_str());
		}
	}

	void SetFrameConstants(sl::FrameToken* currentFrameToken)
	{
		using namespace sl;

		Constants constants = {};

		//expects row major unjittered matrices
		constants.cameraViewToClip = *reinterpret_cast<sl::float4x4*>(&_commonData->CameraViewToClip);
		constants.clipToCameraView = *reinterpret_cast<sl::float4x4*>(&_commonData->ClipToCameraView);
		constants.clipToPrevClip = *reinterpret_cast<sl::float4x4*>(&_commonData->ClipToPrevClip);
		constants.prevClipToClip = *reinterpret_cast<sl::float4x4*>(&_commonData->PrevClipToClip);

		constants.cameraNear = _commonData->ActiveCameraNearPlane;
		constants.cameraFar = _commonData->ActiveCameraFarPlane;
		constants.cameraFOV = XMConvertToRadians(_commonData->ActiveCameraFOV);
		constants.cameraAspectRatio = static_cast<float>(_commonData->WindowWidth) / _commonData->WindowHeight;

		constants.jitterOffset = { _commonData->ActiveCameraJitterOffsetX, _commonData->ActiveCameraJitterOffsetY };
		constants.mvecScale = { 1.f, 1.f };

		constants.depthInverted = Boolean::eTrue;
		constants.cameraMotionIncluded = Boolean::eFalse;
		constants.motionVectors3D = Boolean::eFalse;
		constants.reset = Boolean::eFalse;
		constants.orthographicProjection = Boolean::eFalse;
		constants.motionVectorsDilated = Boolean::eFalse;
		constants.motionVectorsJittered = Boolean::eFalse;

		Result result = slSetConstants(constants, *currentFrameToken, _viewportHandle);
		if (result != Result::eOk)
		{
			std::string errorMsg = "DLSS: Failed to set constants: " + SLResultToString(result) + "\n";
			OutputDebugStringA(errorMsg.c_str());
		}
	}

	void CreateDLSSFeature()
	{
		using namespace sl;
		// Create DLSS feature
		DLSSOptions dlssOptions = {};
		dlssOptions.mode = _DLSSQualityMode;
		dlssOptions.outputWidth = _commonData->WindowWidth;
		dlssOptions.outputHeight = _commonData->WindowHeight;
		dlssOptions.sharpness = 0.25f;
		dlssOptions.colorBuffersHDR = Boolean::eFalse;

		Result result = slDLSSSetOptions(_viewportHandle, dlssOptions);
		if (result != Result::eOk)
		{
			std::string errorMsg = "ERROR: Failed to set DLSS options: " + SLResultToString(result) + "\n";
			OutputDebugStringA(errorMsg.c_str());
			return;
		}

		result = slAllocateResources(nullptr, kFeatureDLSS, _viewportHandle);
		if (result != Result::eOk)
		{
			std::string errorMsg = "ERROR: Failed to allocate DLSS resources: " + SLResultToString(result) + "\n";
			OutputDebugStringA(errorMsg.c_str());
			return;
		}
	}

	std::string SLResultToString(sl::Result result)
	{
		switch (result)
		{
		case sl::Result::eOk: return "OK";
		case sl::Result::eErrorIO: return "ERROR_IO";
		case sl::Result::eErrorDriverOutOfDate: return "ERROR_DRIVER_OUT_OF_DATE";
		case sl::Result::eErrorOSOutOfDate: return "ERROR_OS_OUT_OF_DATE";
		case sl::Result::eErrorOSDisabledHWS: return "ERROR_OS_DISABLED_HWS";
		case sl::Result::eErrorDeviceNotCreated: return "ERROR_DEVICE_NOT_CREATED";
		case sl::Result::eErrorNoSupportedAdapterFound: return "ERROR_NO_SUPPORTED_ADAPTER_FOUND";
		case sl::Result::eErrorAdapterNotSupported: return "ERROR_ADAPTER_NOT_SUPPORTED";
		case sl::Result::eErrorNoPlugins: return "ERROR_NO_PLUGINS";
		case sl::Result::eErrorVulkanAPI: return "ERROR_VULKAN_API";
		case sl::Result::eErrorDXGIAPI: return "ERROR_DXGI_API";
		case sl::Result::eErrorD3DAPI: return "ERROR_D3D_API";
		case sl::Result::eErrorNRDAPI: return "ERROR_NRD_API";
		case sl::Result::eErrorNVAPI: return "ERROR_NV_API";
		case sl::Result::eErrorReflexAPI: return "ERROR_REFLEX_API";
		case sl::Result::eErrorNGXFailed: return "ERROR_NGX_FAILED";
		case sl::Result::eErrorJSONParsing: return "ERROR_JSON_PARSING";
		case sl::Result::eErrorMissingProxy: return "ERROR_MISSING_PROXY";
		case sl::Result::eErrorMissingResourceState: return "ERROR_MISSING_RESOURCE_STATE";
		case sl::Result::eErrorInvalidIntegration: return "ERROR_INVALID_INTEGRATION";
		case sl::Result::eErrorMissingInputParameter: return "ERROR_MISSING_INPUT_PARAMETER";
		case sl::Result::eErrorNotInitialized: return "ERROR_NOT_INITIALIZED";
		case sl::Result::eErrorComputeFailed: return "ERROR_COMPUTE_FAILED";
		case sl::Result::eErrorInitNotCalled: return "ERROR_INIT_NOT_CALLED";
		case sl::Result::eErrorExceptionHandler: return "ERROR_EXCEPTION_HANDLER";
		case sl::Result::eErrorInvalidParameter: return "ERROR_INVALID_PARAMETER";
		case sl::Result::eErrorMissingConstants: return "ERROR_MISSING_CONSTANTS";
		case sl::Result::eErrorDuplicatedConstants: return "ERROR_DUPLICATED_CONSTANTS";
		case sl::Result::eErrorMissingOrInvalidAPI: return "ERROR_MISSING_OR_INVALID_API";
		case sl::Result::eErrorCommonConstantsMissing: return "ERROR_COMMON_CONSTANTS_MISSING";
		case sl::Result::eErrorUnsupportedInterface: return "ERROR_UNSUPPORTED_INTERFACE";
		case sl::Result::eErrorFeatureMissing: return "ERROR_FEATURE_MISSING";
		case sl::Result::eErrorFeatureNotSupported: return "ERROR_FEATURE_NOT_SUPPORTED";
		case sl::Result::eErrorFeatureMissingHooks: return "ERROR_FEATURE_MISSING_HOOKS";
		case sl::Result::eErrorFeatureFailedToLoad: return "ERROR_FEATURE_FAILED_TO_LOAD";
		case sl::Result::eErrorFeatureWrongPriority: return "ERROR_FEATURE_WRONG_PRIORITY";
		case sl::Result::eErrorFeatureMissingDependency: return "ERROR_FEATURE_MISSING_DEPENDENCY";
		case sl::Result::eErrorFeatureManagerInvalidState: return "ERROR_FEATURE_MANAGER_INVALID_STATE";
		case sl::Result::eErrorInvalidState: return "ERROR_INVALID_STATE";
		case sl::Result::eWarnOutOfVRAM: return "WARN_OUT_OF_VRAM";
		default: return "UNKNOWN_RESULT";
		}
	}
};
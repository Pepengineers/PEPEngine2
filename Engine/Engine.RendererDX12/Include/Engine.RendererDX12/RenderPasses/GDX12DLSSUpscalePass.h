#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include "Common/GameTimer.h"

// HOLY SHIT
// we need this crazy include since SL's free() function conflits with CRT library's free() function
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
#include "Engine.RendererDX12/GDX12StreamlineSDK.h"

class GDX12DLSSUpscalePass : public GDX12RenderPass
{
public:
	GDX12DLSSUpscalePass() : IN_DepthBuffer(nullptr), IN_MotionVectors(nullptr), _viewportHandle(1337),
		_DLSSQualityMode(sl::DLSSMode::eMaxQuality)
	{ _flags = RENDER_PASS_FLAG_UPSCALER | RENDER_PASS_FLAG_USE_JITTER | RENDER_PASS_FLAG_USE_STREAMLINE; }

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

		CheckDLSSSupport();
		QueryRenderTargetResolution();
		CreateDLSSFeature();
	}

	void Execute(GDX12CommandList* cmdList) override
	{
		using namespace sl;

		cmdList->BeginPixEvent("DLSS Upscale Pass", Colors::Blue);

		FrameToken* currentFrame = nullptr;
		Result result = GDX12StreamlineSDK::Get().GetNewFrameToken(currentFrame);
		if (result != Result::eOk || currentFrame == nullptr)
		{
			std::string errorMsg = "DLSS: Failed to get a frame token: " + SLResultToString(result) + "\n";
			OutputDebugStringA(errorMsg.c_str());
			cmdList->EndPixEvent();
			return;
		}

		SetConstants(currentFrame);

		GDX12Texture* depthStencil = IN_DepthBuffer->GetTexture();
		GDX12Texture* motionVectors = IN_MotionVectors->GetTexture();

		Resource depthResource = Resource{ ResourceType::eTex2d,
		depthStencil->GetResource()->D3DResource.Get(),
		nullptr, nullptr, static_cast<uint32_t>(depthStencil->GetResource()->GetCurrentState()) };

		Resource mvecResource = Resource{ ResourceType::eTex2d,
			motionVectors->GetResource()->D3DResource.Get(),
			nullptr, nullptr, static_cast<uint32_t>(motionVectors->GetResource()->GetCurrentState()) };

		Extent fullExtent{};
		fullExtent.top = 0;
		fullExtent.left = 0;
		fullExtent.width = _commonData->DownscaledWidth;
		fullExtent.height = _commonData->DownscaledHeight;

		Extent outputExtent{};
		outputExtent.top = 0;
		outputExtent.left = 0;
		outputExtent.width = _commonData->WindowWidth;
		outputExtent.height = _commonData->WindowHeight;

		ResourceTag depthTag{ &depthResource, kBufferTypeDepth, ResourceLifecycle::eValidUntilEvaluate, &fullExtent };
		ResourceTag mvecTag{ &mvecResource, kBufferTypeMotionVectors, ResourceLifecycle::eValidUntilEvaluate, &fullExtent };

		for (int i = 0; i < _numOutputs; i++)
		{
			GDX12Texture* inputTexture = IN_Textures[i]->GetTexture();
			GDX12Texture* upscaledTexture = OUT_UpscaledTextures[i].get();

			Resource inputResource = Resource{ ResourceType::eTex2d,
				inputTexture->GetResource()->D3DResource.Get(),
				nullptr, nullptr, static_cast<uint32_t>(inputTexture->GetResource()->GetCurrentState()) };

			Resource outputResource = Resource{ ResourceType::eTex2d,
				upscaledTexture->GetResource()->D3DResource.Get(),
				nullptr, nullptr, static_cast<uint32_t>(upscaledTexture->GetResource()->GetCurrentState()) };

			ResourceTag inputTag{ &inputResource, kBufferTypeScalingInputColor, ResourceLifecycle::eValidUntilEvaluate, &fullExtent };
			ResourceTag outputTag{ &outputResource, kBufferTypeScalingOutputColor, ResourceLifecycle::eValidUntilEvaluate, &outputExtent };

			ResourceTag tags[] = { depthTag, mvecTag, inputTag, outputTag };
			result = GDX12StreamlineSDK::Get().SetTagForFrame(*currentFrame, _viewportHandle, tags, _countof(tags), cmdList->GetCommandList().Get());
			if (result != Result::eOk)
			{
				std::string errorMsg = "DLSS: Failed to set tags: " + SLResultToString(result) + "\n";
				OutputDebugStringA(errorMsg.c_str());
			}

			const BaseStructure* inputs[] = { &_viewportHandle };
			result = GDX12StreamlineSDK::Get().EvaluateFeature(kFeatureDLSS, *currentFrame, inputs, _countof(inputs), cmdList->GetCommandList().Get());
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
		GDX12StreamlineSDK::Get().FreeResources(sl::kFeatureDLSS, _viewportHandle);
		GDX12StreamlineSDK::Get().DLSSSetOptions(_viewportHandle, sl::DLSSOptions{});
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
		Result result = GDX12StreamlineSDK::Get().DLSSGetOptimalSettings(dlssOptions, optimalSettings);
		if (result != Result::eOk)
		{
			std::string errorMsg = "DLSS: Failed to get optimal settings: " + SLResultToString(result) + "\n";
			OutputDebugStringA(errorMsg.c_str());
			_commonData->DownscaledWidth = _commonData->WindowWidth;
			_commonData->DownscaledHeight = _commonData->WindowHeight;
			return;
		}

		if (optimalSettings.optimalRenderWidth == 0 || optimalSettings.optimalRenderHeight == 0)
		{
			OutputDebugStringA("DLSS: Optimal settings returned a zero-sized render resolution, falling back to full-resolution rendering.\n");
			_commonData->DownscaledWidth = _commonData->WindowWidth;
			_commonData->DownscaledHeight = _commonData->WindowHeight;
			return;
		}

		_commonData->DownscaledWidth = optimalSettings.optimalRenderWidth;
		_commonData->DownscaledHeight = optimalSettings.optimalRenderHeight;
	}

	void ClearDenendencies() override
	{
		GDX12RenderPass::ClearDenendencies();
		GDX12StreamlineSDK::Get().FreeResources(sl::kFeatureDLSS, _viewportHandle);
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

	bool CheckDLSSSupport()
	{
		using namespace sl;

		LUID adapterLuid = _resources->Device->GetDevice()->GetAdapterLuid();
		AdapterInfo adapterInfo = {};
		adapterInfo.deviceLUID = reinterpret_cast<uint8_t*>(&adapterLuid);
		adapterInfo.deviceLUIDSizeInBytes = sizeof(adapterLuid);

		Result result = GDX12StreamlineSDK::Get().IsFeatureSupported(kFeatureDLSS, adapterInfo);
		if (result == Result::eOk) { return true; }

		std::string errorMsg = "ERROR: DLSS Feature is not supported on the current adapter: " + SLResultToString(result) + "\n";
		OutputDebugStringA(errorMsg.c_str());
		return false;
	}

	void SetConstants(sl::FrameToken* currentFrameToken)
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

		constants.jitterOffset = { -_commonData->ActiveCameraJitterOffsetX, -_commonData->ActiveCameraJitterOffsetY };
		constants.mvecScale = { 1.f, 1.f };

		constants.depthInverted = Boolean::eTrue;
		constants.cameraMotionIncluded = Boolean::eTrue;
		constants.motionVectors3D = Boolean::eFalse;
		constants.reset = Boolean::eFalse;
		constants.orthographicProjection = Boolean::eFalse;
		constants.motionVectorsDilated = Boolean::eFalse;
		constants.motionVectorsJittered = Boolean::eFalse;

		Result result = GDX12StreamlineSDK::Get().SetConstants(constants, *currentFrameToken, _viewportHandle);
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

		Result result = GDX12StreamlineSDK::Get().DLSSSetOptions(_viewportHandle, dlssOptions);
		if (result != Result::eOk)
		{
			std::string errorMsg = "ERROR: Failed to set DLSS options: " + SLResultToString(result) + "\n";
			OutputDebugStringA(errorMsg.c_str());
			return;
		}

	}

	std::string SLResultToString(sl::Result result)
	{
		return GDX12StreamlineSDK::ResultToString(result);
	}
};

#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"

#include "Engine.RendererDX12/GDX12StreamlineSDK.h"
#include <stdexcept>
#include <algorithm>

class GDX12NISUpscalePass : public GDX12RenderPass
{
public:
	explicit GDX12NISUpscalePass(float ratio = 1.5f) : IN_DepthBuffer(nullptr), IN_MotionVectors(nullptr), _viewportHandle(1337),
		_sharpness(0.1f), _ratio(ratio)
	{ _numInputs = 2; _flags = RENDER_PASS_FLAG_UPSCALER | RENDER_PASS_FLAG_USE_STREAMLINE; }

	virtual void SetInputs(std::vector<IRenderPassLink*> inputs) override
	{
		GDX12RenderPass::SetInputs(std::move(inputs));
		MatchOutputCount(_inputs.size() > 2 ? static_cast<UINT>(_inputs.size() - 2) : 0);
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

		CheckNISSupport();
		QueryRenderTargetResolution();
		CreateNISFeature();
	}

	void Execute(GDX12CommandList* cmdList) override
	{
		using namespace sl;

		GDX12_SCOPED_EVENT(cmdList, "NIS Upscale Pass", Colors::Blue);

		FrameToken* currentFrame = _commonData->StreamlineFrame;
        Result result = currentFrame ? Result::eOk : GDX12StreamlineSDK::Get().GetNewFrameToken(currentFrame);
		if (result != Result::eOk || currentFrame == nullptr)
		{
			std::string errorMsg = "NIS: Failed to get a frame token: " + SLResultToString(result) + "\n";
			throw std::runtime_error(errorMsg);
		}

		Extent inputExtent{};
		inputExtent.top = 0;
		inputExtent.left = 0;
		inputExtent.width = _commonData->DownscaledWidth;
		inputExtent.height = _commonData->DownscaledHeight;

		Extent outputExtent{};
		outputExtent.top = 0;
		outputExtent.left = 0;
		outputExtent.width = _commonData->WindowWidth;
		outputExtent.height = _commonData->WindowHeight;

		for (UINT i = 0; i < _numOutputs; i++)
		{
			GDX12Texture* inputTexture = IN_Textures[i]->GetTexture();
			GDX12Texture* upscaledTexture = OUT_UpscaledTextures[i].get();
            cmdList->ResourceBarrier({ inputTexture->GetResource()->GetNonPixelShaderResourceBarrier(),
                upscaledTexture->GetResource()->GetUnorderedAccessBarrier() });

			Resource inputResource = Resource{ ResourceType::eTex2d,
				inputTexture->GetResource()->D3DResource.Get(),
				nullptr, nullptr, static_cast<uint32_t>(inputTexture->GetResource()->GetCurrentState()) };

			Resource outputResource = Resource{ ResourceType::eTex2d,
				upscaledTexture->GetResource()->D3DResource.Get(),
				nullptr, nullptr, static_cast<uint32_t>(upscaledTexture->GetResource()->GetCurrentState()) };

			ResourceTag inputTag{ &inputResource, kBufferTypeScalingInputColor, ResourceLifecycle::eValidUntilEvaluate, &inputExtent };
			ResourceTag outputTag{ &outputResource, kBufferTypeScalingOutputColor, ResourceLifecycle::eValidUntilEvaluate, &outputExtent };

			ResourceTag tags[] = { inputTag, outputTag };
			result = GDX12StreamlineSDK::Get().SetTagForFrame(*currentFrame, _viewportHandle, tags, _countof(tags), cmdList->GetCommandList().Get());
			if (result != Result::eOk)
			{
				std::string errorMsg = "NIS: Failed to set tags: " + SLResultToString(result) + "\n";
				throw std::runtime_error(errorMsg);
			}

			const BaseStructure* inputs[] = { &_viewportHandle };
			result = GDX12StreamlineSDK::Get().EvaluateFeature(kFeatureNIS, *currentFrame, inputs, _countof(inputs), cmdList->GetCommandList().Get());
            cmdList->InvalidateCachedState();
			if (result != Result::eOk)
			{
				std::string errorMsg = "NIS EVALUATE ERROR: " + SLResultToString(result) + "\n";
				throw std::runtime_error(errorMsg);
			}
            cmdList->ResourceBarrier({ upscaledTexture->GetResource()->GetUAVBarrier() });
		}

	}

	void Resize() override
	{
		for (auto& texture : OUT_UpscaledTextures) { texture->Resize(_commonData->WindowWidth, _commonData->WindowHeight); }
		GDX12StreamlineSDK::Get().FreeResources(sl::kFeatureNIS, _viewportHandle);
		CreateNISFeature();
	}

	void QueryRenderTargetResolution() override
	{
		const float UpscaleRatio = _ratio;
		_commonData->DownscaledWidth = static_cast<UINT>(static_cast<float>(_commonData->WindowWidth) / UpscaleRatio);
		_commonData->DownscaledHeight = static_cast<UINT>(static_cast<float>(_commonData->WindowHeight) / UpscaleRatio);
	}

	void ClearDenendencies() override
	{
		GDX12RenderPass::ClearDenendencies();
		GDX12StreamlineSDK::Get().FreeResources(sl::kFeatureNIS, _viewportHandle);

		sl::NISOptions nisOptions = {};
		nisOptions.mode = sl::NISMode::eOff;
		GDX12StreamlineSDK::Get().NISSetOptions(_viewportHandle, nisOptions);

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
	float _sharpness;
    float _ratio;

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

	void CheckNISSupport()
	{
		using namespace sl;

		LUID adapterLuid = _resources->Device->GetDevice()->GetAdapterLuid();
		AdapterInfo adapterInfo = {};
		adapterInfo.deviceLUID = reinterpret_cast<uint8_t*>(&adapterLuid);
		adapterInfo.deviceLUIDSizeInBytes = sizeof(adapterLuid);

		Result result = GDX12StreamlineSDK::Get().IsFeatureSupported(kFeatureNIS, adapterInfo);
		if (result == Result::eOk) { return; }

		std::string errorMsg = "ERROR: NIS Feature is not supported on the current adapter: " + SLResultToString(result) + "\n";
		throw std::runtime_error(errorMsg);
	}

	void CreateNISFeature()
	{
		using namespace sl;

		NISOptions nisOptions = {};
		nisOptions.mode = NISMode::eScaler;
		nisOptions.hdrMode = NISHDR::eLinear;
		nisOptions.sharpness = _sharpness;

		Result result = GDX12StreamlineSDK::Get().NISSetOptions(_viewportHandle, nisOptions);
		if (result != Result::eOk)
		{
			std::string errorMsg = "ERROR: Failed to set NIS options: " + SLResultToString(result) + "\n";
			throw std::runtime_error(errorMsg);
		}
	}

	std::string SLResultToString(sl::Result result)
	{
		return GDX12StreamlineSDK::ResultToString(result);
	}
};

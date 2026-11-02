#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

#include "Engine.RendererDX12/GDX12StreamlineSDK.h"

class GDX12NISUpscalePass : public GDX12RenderPass
{
public:
	GDX12NISUpscalePass() : IN_DepthBuffer(nullptr), IN_MotionVectors(nullptr), _viewportHandle(1337),
		_sharpness(0.1f)
	{ _flags = RENDER_PASS_FLAG_UPSCALER | RENDER_PASS_FLAG_USE_STREAMLINE; }

	virtual void SetInputs(std::vector<IRenderPassLink*> inputs) override
	{
		_inputs = inputs;
		_numInputs = 2;
		MatchOutputCount(inputs.size() > 2 ? static_cast<UINT>(inputs.size() - 2) : 0);
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

		CheckNISSupport();
		QueryRenderTargetResolution();
		CreateNISFeature();
	}

	void Execute(GDX12CommandList* cmdList) override
	{
		using namespace sl;

		cmdList->BeginPixEvent("NIS Upscale Pass", Colors::Blue);

		FrameToken* currentFrame = nullptr;
		Result result = GDX12StreamlineSDK::Get().GetNewFrameToken(currentFrame);
		if (result != Result::eOk || currentFrame == nullptr)
		{
			std::string errorMsg = "NIS: Failed to get a frame token: " + SLResultToString(result) + "\n";
			OutputDebugStringA(errorMsg.c_str());
			cmdList->EndPixEvent();
			return;
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
				OutputDebugStringA(errorMsg.c_str());
			}

			const BaseStructure* inputs[] = { &_viewportHandle };
			result = GDX12StreamlineSDK::Get().EvaluateFeature(kFeatureNIS, *currentFrame, inputs, _countof(inputs), cmdList->GetCommandList().Get());
			if (result != Result::eOk)
			{
				std::string errorMsg = "NIS EVALUATE ERROR: " + SLResultToString(result) + "\n";
				OutputDebugStringA(errorMsg.c_str());
			}
		}

		cmdList->EndPixEvent();
	}

	void Resize() override
	{
		for (auto& texture : OUT_UpscaledTextures) { texture->Resize(_commonData->WindowWidth, _commonData->WindowHeight); }
		GDX12StreamlineSDK::Get().FreeResources(sl::kFeatureNIS, _viewportHandle);
		CreateNISFeature();
	}

	void QueryRenderTargetResolution() override
	{
		constexpr float UpscaleRatio = 1.5f;
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

	bool CheckNISSupport()
	{
		using namespace sl;

		LUID adapterLuid = _resources->Device->GetDevice()->GetAdapterLuid();
		AdapterInfo adapterInfo = {};
		adapterInfo.deviceLUID = reinterpret_cast<uint8_t*>(&adapterLuid);
		adapterInfo.deviceLUIDSizeInBytes = sizeof(adapterLuid);

		Result result = GDX12StreamlineSDK::Get().IsFeatureSupported(kFeatureNIS, adapterInfo);
		if (result == Result::eOk) { return true; }

		std::string errorMsg = "ERROR: NIS Feature is not supported on the current adapter: " + SLResultToString(result) + "\n";
		OutputDebugStringA(errorMsg.c_str());
		return false;
	}

	void CreateNISFeature()
	{
		using namespace sl;

		NISOptions nisOptions = {};
		nisOptions.mode = NISMode::eScaler;
		nisOptions.hdrMode = NISHDR::eNone;
		nisOptions.sharpness = _sharpness;

		Result result = GDX12StreamlineSDK::Get().NISSetOptions(_viewportHandle, nisOptions);
		if (result != Result::eOk)
		{
			std::string errorMsg = "ERROR: Failed to set NIS options: " + SLResultToString(result) + "\n";
			OutputDebugStringA(errorMsg.c_str());
		}
	}

	std::string SLResultToString(sl::Result result)
	{
		return GDX12StreamlineSDK::ResultToString(result);
	}
};

#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include "Common/GameTimer.h"

#include "xess/xess.h"
#include "xess/xess_d3d12.h"

class GDX12XeSSUpscalePass : public GDX12RenderPass
{
public:
	GDX12XeSSUpscalePass() : IN_DepthBuffer(nullptr), IN_MotionVectors(nullptr)
	{ _flags = RENDER_PASS_FLAG_UPSCALER; }

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

		for (int i = 0; i < _numOutputs; i++)
		{
			IN_Textures[i] = _inputs[i + 2];
			GDX12Texture* inputTexture = IN_Textures[i]->GetTexture();
			TextureDesc1.Format = TextureDesc1.RTVDesc.Format = TextureDesc1.SRVDesc.Format = inputTexture->GetFormat();
			TextureDesc1.SRVHeapIndex = _resources->SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
			OUT_UpscaledTextures[i]->Initialize(TextureDesc1);
		}

	}

	// Init with window size
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

		for (int i = 0; i < _numOutputs; i++)
		{
			GDX12Texture* inputTexture = IN_Textures[i]->GetTexture();
			GDX12Texture* upscaledTexture = OUT_UpscaledTextures[i].get();

		}
		cmdList->EndPixEvent();
	}

	void Resize() override
	{
		for (auto& texture : OUT_UpscaledTextures) { texture->Resize(_commonData->WindowWidth, _commonData->WindowHeight); }
	}

	void QueryRenderTargetResolution() override
	{
		xess_2d_t targetResolution = { _commonData->WindowWidth, _commonData->WindowHeight } ;
		xess_2d_t downscaledResolution;
		//xessGetOptimalInputResolution(_XeSSContext, &targetResolution, XeSSQualityMode, &downscaledResolution);
	}

	void ClearDenendencies() override
	{
		GDX12RenderPass::ClearDenendencies();

		IN_DepthBuffer = nullptr;
		IN_MotionVectors = nullptr;
		IN_Textures.clear();
		OUT_UpscaledTextures.clear();
	}

	xess_quality_settings_t XeSSQualityMode = XESS_QUALITY_SETTING_QUALITY;

private:
	IRenderPassLink* IN_DepthBuffer;
	IRenderPassLink* IN_MotionVectors;
	std::vector<IRenderPassLink*> IN_Textures;
	std::vector<std::unique_ptr<GDX12Texture>> OUT_UpscaledTextures;
	xess_context_handle_t _XeSSContext;

	void BuildXeSSContext()
	{
		xess_result_t createContextResult = xessD3D12CreateContext(_resources->Device->GetDevice().Get(), &_XeSSContext);
		if (createContextResult != XESS_RESULT_SUCCESS)
		{
			std::string msg = "ERROR: XeSS context not created: " + XeSSResultToString(createContextResult) + "\n";
			OutputDebugStringA(msg.c_str());
		}

		xess_d3d12_init_params_t initParams = {};
		initParams.outputResolution.x = _commonData->WindowWidth;
		initParams.outputResolution.y = _commonData->WindowHeight;
		initParams.initFlags = XESS_INIT_FLAG_INVERTED_DEPTH;

		xess_result_t initResult = xessD3D12Init(_XeSSContext, &initParams);
		if (initResult != XESS_RESULT_SUCCESS) 
		{
			std::string msg = "ERROR: XeSS context not initialized: " + XeSSResultToString(initResult) + "\n";
			OutputDebugStringA(msg.c_str());
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
#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include "xess/xess.h"
#include "xess/xess_d3d12.h"

class GDX12XeSSUpscalePass : public GDX12RenderPass
{
public:
	static constexpr const char* Source = "Source";
	static constexpr const char* Depth = "Depth";
	static constexpr const char* Velocity = "Velocity";
	static constexpr const char* Color = "Color";

	explicit GDX12XeSSUpscalePass(xess_quality_settings_t quality = XESS_QUALITY_SETTING_ULTRA_QUALITY_PLUS)
		: _quality(quality)
	{
		if (quality < XESS_QUALITY_SETTING_ULTRA_PERFORMANCE || quality > XESS_QUALITY_SETTING_AA)
		{ throw std::invalid_argument("ERROR: Given XeSS quality mode is unsupported. Pass an xess_quality_settings_t value from "
			"XESS_QUALITY_SETTING_ULTRA_PERFORMANCE through XESS_QUALITY_SETTING_AA."); }
		_flags = RENDER_PASS_FLAG_UPSCALER | RENDER_PASS_FLAG_USE_JITTER;
	}

	~GDX12XeSSUpscalePass() override
	{ ReleaseContext(); }

	void Describe(PassDescription& s) const override
	{
		auto& source = s.AddInputTexture(Source);
		source.Formats = {DXGI_FORMAT_R8G8B8A8_UNORM};
		source.Usages = ResourceUsage::NonPixelShaderRead;
		auto& depth = s.AddInputTexture(Depth);
		depth.Formats = {DXGI_FORMAT_D32_FLOAT};
		depth.Usages = ResourceUsage::NonPixelShaderRead;
		auto& velocity = s.AddInputTexture(Velocity);
		velocity.Formats = {DXGI_FORMAT_R16G16_FLOAT};
		velocity.Usages = ResourceUsage::NonPixelShaderRead;
		auto& color = s.AddOutputTexture(Color);
		color.Formats = {DXGI_FORMAT_R8G8B8A8_UNORM};
		color.Size = Extent::Display();
		color.Usages = ResourceUsage::UnorderedAccess;
		s.RequireSameTextureSize({"Source", "Depth", "Velocity"});
	}

	void ResolveResourceRequirements(PassResourceRequirementsContext& context) override
	{
		ReleaseContext();
		const auto displaySize = context.GetDisplaySize();
		Check(xessD3D12CreateContext(context.GetDevice()->GetDevice().Get(), &_context), "create context");
		xess_d3d12_init_params_t params = {};
		params.outputResolution = {displaySize.x, displaySize.y};
		params.qualitySetting = _quality;
		params.initFlags = XESS_INIT_FLAG_INVERTED_DEPTH | XESS_INIT_FLAG_LDR_INPUT_COLOR | XESS_INIT_FLAG_USE_NDC_VELOCITY;
		Check(xessD3D12Init(_context, &params), "initialize context");
		xess_2d_t recommended{}, minimum{}, maximum{};
		Check(xessGetOptimalInputResolution(_context, &params.outputResolution, _quality,
			&recommended, &minimum, &maximum), "query input resolution");
		const UInt2 renderSize = {recommended.x, recommended.y};
		if (!renderSize.x || !renderSize.y) { throw std::runtime_error("ERROR: XeSS returned a zero or invalid input resolution. Use non-zero display dimensions and a supported XeSS "
			"quality mode, then recreate the XeSS context."); }
		context.RequireTextureSize(Source, renderSize);
		context.RequireTextureSize(Depth, renderSize);
		context.RequireTextureSize(Velocity, renderSize);
		_resetHistory = true;
	}

	void Execute(PassExecutionContext& context) override
	{
		const auto renderSize = context.GetInputResourceDescription(Source).GetSize();
		auto* cmdList = context.GetCommandList();
		const auto* frame = context.GetFrameData();
		xess_d3d12_execute_params_t params = {};
		params.pColorTexture = context.GetInputTexture(Source).GetTexture()->GetResource()->D3DResource.Get();
		params.pDepthTexture = context.GetInputTexture(Depth).GetTexture()->GetResource()->D3DResource.Get();
		params.pVelocityTexture = context.GetInputTexture(Velocity).GetTexture()->GetResource()->D3DResource.Get();
		params.pOutputTexture = context.GetOutputTexture(Color).GetTexture()->GetResource()->D3DResource.Get();
		params.jitterOffsetX = -frame->ActiveCameraJitterOffsetX;
		params.jitterOffsetY = -frame->ActiveCameraJitterOffsetY;
		params.inputWidth = renderSize.x;
		params.inputHeight = renderSize.y;
		params.resetHistory = _resetHistory;
		params.exposureScale = 1.f;
		cmdList->BeginPixEvent("XeSS Upscale", Colors::Blue);
		Check(xessD3D12Execute(_context, cmdList->GetCommandList().Get(), &params), "execute");
		cmdList->EndPixEvent();
		_resetHistory = false;
	}

	void OnResourcesRecreated(PassInitContext&) override
	{ _resetHistory = true; }

private:
	xess_context_handle_t _context = nullptr;
	const xess_quality_settings_t _quality;
	bool _resetHistory = true;

	void ReleaseContext()
	{
		if (_context) { xessDestroyContext(_context); _context = nullptr; }
	}

	void Check(xess_result_t result, const char* operation)
	{
		if (result != XESS_RESULT_SUCCESS)
		{ throw std::runtime_error(std::string("ERROR: XeSS failed to ") + operation + " (" + XeSSResultToString(result) + "). Check the reported SDK result, XeSS context initialization, and device support."); }
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

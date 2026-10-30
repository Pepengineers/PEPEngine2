#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include "Common/GameTimer.h"
#include "ffx-api/ffx_api.h"
#include "ffx-api/ffx_upscale.h"
#include "ffx-api/dx12/ffx_api_dx12.h"

class GDX12FSRUpscalePass : public GDX12RenderPass
{
public:
	static constexpr const char* InColorTexture = "InColorTexture";
	static constexpr const char* InDepthBuffer = "InDepthBuffer";
	static constexpr const char* InVelocityBuffer = "InVelocityBuffer";
	static constexpr const char* OutColorTexture = "OutColorTexture";

	explicit GDX12FSRUpscalePass(FfxApiUpscaleQualityMode quality = FFX_UPSCALE_QUALITY_MODE_QUALITY)
		: _quality(quality)
	{
		if (quality < FFX_UPSCALE_QUALITY_MODE_NATIVEAA || quality > FFX_UPSCALE_QUALITY_MODE_ULTRA_PERFORMANCE)
		{ throw std::invalid_argument("ERROR: Given FSR quality mode is unsupported. Pass a FfxApiUpscaleQualityMode from "
			"FFX_UPSCALE_QUALITY_MODE_NATIVEAA through FFX_UPSCALE_QUALITY_MODE_ULTRA_PERFORMANCE."); }
		_flags = RENDER_PASS_FLAG_UPSCALER | RENDER_PASS_FLAG_USE_JITTER;
	}

	~GDX12FSRUpscalePass() override
	{ ReleaseContext(); }

	void Describe(PassDescription& s) const override
	{
		const auto read = ResourceUsage::PixelShaderRead | ResourceUsage::NonPixelShaderRead;
		auto& source = s.AddInputTexture(InColorTexture);
		source.Formats = {DXGI_FORMAT_R8G8B8A8_UNORM};
		source.Usages = read;
		auto& depth = s.AddInputTexture(InDepthBuffer);
		depth.Formats = {DXGI_FORMAT_D32_FLOAT};
		depth.Usages = read;
		auto& velocity = s.AddInputTexture(InVelocityBuffer);
		velocity.Formats = {DXGI_FORMAT_R16G16_FLOAT};
		velocity.Usages = read;
		auto& color = s.AddOutputTexture(OutColorTexture);
		color.Formats = {DXGI_FORMAT_R8G8B8A8_UNORM};
		color.Size = Extent::Display();
		color.Usages = ResourceUsage::UnorderedAccess;
		s.RequireSameTextureSize({InColorTexture, InDepthBuffer, InVelocityBuffer});
	}

	void ResolveResourceRequirements(PassResourceRequirementsContext& context) override
	{
		ReleaseContext();
		const auto displaySize = context.GetDisplaySize();
		ffxCreateBackendDX12Desc backend = {};
		backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
		backend.device = context.GetDevice()->GetDevice().Get();
		ffxCreateContextDescUpscale upscale = {};
		upscale.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
		upscale.header.pNext = &backend.header;
		upscale.maxRenderSize = {displaySize.x, displaySize.y};
		upscale.maxUpscaleSize = {displaySize.x, displaySize.y};
		upscale.flags = FFX_UPSCALE_ENABLE_DEBUG_CHECKING | FFX_UPSCALE_ENABLE_DEPTH_INVERTED |
			FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
		Check(ffxCreateContext(&_context, &upscale.header, nullptr), "create context");
		ffxQueryDescUpscaleGetRenderResolutionFromQualityMode query = {};
		query.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETRENDERRESOLUTIONFROMQUALITYMODE;
		query.displayWidth = displaySize.x;
		query.displayHeight = displaySize.y;
		query.qualityMode = _quality;
		UInt2 renderSize;
		query.pOutRenderWidth = &renderSize.x;
		query.pOutRenderHeight = &renderSize.y;
		Check(ffxQuery(&_context, &query.header), "query input resolution");
		if (!renderSize.x || !renderSize.y) { throw std::runtime_error("ERROR: FSR returned a zero or invalid input resolution. Use non-zero display dimensions and a supported FSR "
			"quality mode, then recreate the FSR context."); }
		context.RequireTextureSize(InColorTexture, renderSize);
		context.RequireTextureSize(InDepthBuffer, renderSize);
		context.RequireTextureSize(InVelocityBuffer, renderSize);
		_resetHistory = true;
	}

	void Execute(PassExecutionContext& context) override
	{
		const auto renderSize = context.GetInputResourceDescription(InColorTexture).GetSize();
		const auto displaySize = context.GetOutputResourceDescription(OutColorTexture).GetSize();
		auto* cmdList = context.GetCommandList();
		const auto* frame = context.GetFrameData();
		ffxDispatchDescUpscale dispatch = {};
		dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
		dispatch.commandList = cmdList->GetCommandList().Get();
		dispatch.color = ffxApiGetResourceDX12(context.GetInputTexture(InColorTexture).GetTexture()->GetResource()->D3DResource.Get(),
			FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
		dispatch.depth = ffxApiGetResourceDX12(context.GetInputTexture(InDepthBuffer).GetTexture()->GetResource()->D3DResource.Get(),
			FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
		dispatch.motionVectors = ffxApiGetResourceDX12(context.GetInputTexture(InVelocityBuffer).GetTexture()->GetResource()->D3DResource.Get(),
			FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
		dispatch.output = ffxApiGetResourceDX12(context.GetOutputTexture(OutColorTexture).GetTexture()->GetResource()->D3DResource.Get(),
			FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
		dispatch.renderSize = {renderSize.x, renderSize.y};
		dispatch.motionVectorScale = {static_cast<float>(renderSize.x), static_cast<float>(renderSize.y)};
		dispatch.upscaleSize = {displaySize.x, displaySize.y};
		dispatch.cameraNear = frame->ActiveCameraFarPlane;
		dispatch.cameraFar = frame->ActiveCameraNearPlane;
		dispatch.cameraFovAngleVertical = XMConvertToRadians(frame->ActiveCameraFOV);
		dispatch.jitterOffset = {-frame->ActiveCameraJitterOffsetX, -frame->ActiveCameraJitterOffsetY};
		dispatch.enableSharpening = true;
		dispatch.sharpness = 1.f;
		dispatch.frameTimeDelta = std::max(frame->GameTimer->DeltaTime() * 1000.f, 1.f);
		dispatch.reset = _resetHistory;
		dispatch.preExposure = 1.f;
		dispatch.viewSpaceToMetersFactor = 1.f;
		dispatch.exposure = ffxApiGetResourceDX12(nullptr, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
		dispatch.reactive = ffxApiGetResourceDX12(nullptr, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
		dispatch.transparencyAndComposition = ffxApiGetResourceDX12(nullptr, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
		cmdList->BeginPixEvent("FSR Upscale", Colors::Blue);
		Check(ffxDispatch(&_context, &dispatch.header), "dispatch");
		cmdList->EndPixEvent();
		_resetHistory = false;
	}

	void OnResourcesRecreated(PassInitContext&) override
	{ _resetHistory = true; }

private:
	const FfxApiUpscaleQualityMode _quality;
	ffxContext _context = nullptr;
	bool _resetHistory = true;

	void ReleaseContext()
	{
		if (_context) { ffxDestroyContext(&_context, nullptr); _context = nullptr; }
	}

	static void Check(ffxReturnCode_t result, const char* operation)
	{
		if (result != FFX_API_RETURN_OK)
		{ throw std::runtime_error(std::string("ERROR: FSR failed to ") + operation + " (" + std::to_string(result) + "). Check the reported SDK result, FSR context initialization, and device support."); }
	}
};

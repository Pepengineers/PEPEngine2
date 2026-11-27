#include "Engine.RendererDX12/RenderPasses/GDX12FSRUpscalePass.h"
#include "Common/GameTimer.h"
#include "ffx-api/dx12/ffx_api_dx12.h"
#include <algorithm>

GDX12FSRUpscalePass::GDX12FSRUpscalePass(FfxApiUpscaleQualityMode quality)
	: _defaultQuality(quality)
{
	ValidateQuality(quality);
	_flags = RENDER_PASS_FLAG_UPSCALER | RENDER_PASS_FLAG_USE_JITTER;
}

GDX12FSRUpscalePass::~GDX12FSRUpscalePass()
{ ReleaseContext(); }

void GDX12FSRUpscalePass::Describe(PassDescription& s) const
{
	s.AddParameter("Quality", _defaultQuality);
	s.AddParameter("EnableSharpening", true);
	s.AddParameter("Sharpness", 1.f);
	const auto read = ResourceUsage::PixelShaderRead | ResourceUsage::NonPixelShaderRead;
	auto& source = s.AddInputTexture(InColorTexture);
	source.Formats = {DXGI_FORMAT_R16G16B16A16_FLOAT};
	source.Usages = read;
	auto& depth = s.AddInputTexture(InDepthBuffer);
	depth.Formats = {DXGI_FORMAT_D32_FLOAT};
	depth.Usages = read;
	auto& velocity = s.AddInputTexture(InVelocityBuffer);
	velocity.Formats = {DXGI_FORMAT_R16G16_FLOAT};
	velocity.Usages = read;
	auto& mask = s.AddInputTexture(InTransparencyMask);
	mask.Formats = {DXGI_FORMAT_R8_UNORM};
	mask.Usages = read;
	mask.IsOptionalInput = true;
	auto& color = s.AddOutputTexture(OutColorTexture);
	color.Formats = {DXGI_FORMAT_R16G16B16A16_FLOAT};
	color.Size = Extent::Display();
	color.Usages = ResourceUsage::UnorderedAccess;
	s.RequireSameTextureSize({InColorTexture, InDepthBuffer, InVelocityBuffer, InTransparencyMask});
}

void GDX12FSRUpscalePass::ResolveResourceRequirements(PassResourceRequirementsContext& context)
{
	const auto quality = context.GetParameter<FfxApiUpscaleQualityMode>("Quality");
	ValidateQuality(quality);
	_enableSharpening = context.GetParameter<bool>("EnableSharpening");
	_sharpness = context.GetParameter<float>("Sharpness");
	if (!(_sharpness >= 0.f && _sharpness <= 1.f))
	{ throw std::invalid_argument("ERROR: FSR Sharpness must be between 0 and 1."); }
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
	upscale.flags = FFX_UPSCALE_ENABLE_DEBUG_CHECKING | FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_DEPTH_INVERTED |
		FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
	Check(ffxCreateContext(&_context, &upscale.header, nullptr), "create context");
	ffxQueryDescUpscaleGetRenderResolutionFromQualityMode query = {};
	query.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETRENDERRESOLUTIONFROMQUALITYMODE;
	query.displayWidth = displaySize.x;
	query.displayHeight = displaySize.y;
	query.qualityMode = quality;
	UInt2 renderSize;
	query.pOutRenderWidth = &renderSize.x;
	query.pOutRenderHeight = &renderSize.y;
	Check(ffxQuery(&_context, &query.header), "query input resolution");
	if (!renderSize.x || !renderSize.y) { throw std::runtime_error("ERROR: FSR returned a zero or invalid input resolution. Use non-zero display dimensions and a supported FSR "
		"quality mode, then recreate the FSR context."); }
	context.RequireTextureSize(InColorTexture, renderSize);
	context.RequireTextureSize(InDepthBuffer, renderSize);
	context.RequireTextureSize(InVelocityBuffer, renderSize);
	if (context.IsInputConnected(InTransparencyMask)) { context.RequireTextureSize(InTransparencyMask, renderSize); }
	_resetHistory = true;
}

void GDX12FSRUpscalePass::Execute(PassExecutionContext& context)
{
	const auto renderSize = context.GetInputResourceDescription(InColorTexture).GetSize();
	const auto displaySize = context.GetOutputResourceDescription(OutColorTexture).GetSize();
	auto* cmdList = context.GetCommandList();
	const auto* frame = context.GetFrameData();
	const auto* mask = context.GetInputTexture(InTransparencyMask);
	ffxDispatchDescUpscale dispatch = {};
	dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
	dispatch.commandList = cmdList->GetCommandList().Get();
	dispatch.color = ffxApiGetResourceDX12(context.GetInputTexture(InColorTexture)->GetTexture()->GetResource()->D3DResource.Get(),
		FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
	dispatch.depth = ffxApiGetResourceDX12(context.GetInputTexture(InDepthBuffer)->GetTexture()->GetResource()->D3DResource.Get(),
		FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
	dispatch.motionVectors = ffxApiGetResourceDX12(context.GetInputTexture(InVelocityBuffer)->GetTexture()->GetResource()->D3DResource.Get(),
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
	dispatch.enableSharpening = _enableSharpening;
	dispatch.sharpness = _sharpness;
	dispatch.frameTimeDelta = (std::max)(frame->GameTimer->DeltaTime() * 1000.f, 1.f);
	dispatch.reset = _resetHistory;
	dispatch.preExposure = 1.f;
	dispatch.viewSpaceToMetersFactor = 1.f;
	dispatch.exposure = ffxApiGetResourceDX12(nullptr, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
	// Coverage controls accumulation; the separate lock-protection mask is not needed here.
	dispatch.reactive = ffxApiGetResourceDX12(mask ? mask->GetTexture()->GetResource()->D3DResource.Get() : nullptr,
		FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
	dispatch.transparencyAndComposition = ffxApiGetResourceDX12(nullptr, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
	cmdList->BeginPixEvent("FSR Upscale", Colors::Blue);
	Check(ffxDispatch(&_context, &dispatch.header), "dispatch");
	cmdList->EndPixEvent();
	_resetHistory = false;
}

void GDX12FSRUpscalePass::OnResourcesRecreated(PassInitContext&)
{ _resetHistory = true; }

void GDX12FSRUpscalePass::ValidateQuality(FfxApiUpscaleQualityMode quality)
{
	if (quality < FFX_UPSCALE_QUALITY_MODE_NATIVEAA || quality > FFX_UPSCALE_QUALITY_MODE_ULTRA_PERFORMANCE)
	{ throw std::invalid_argument("ERROR: Given FSR quality mode is unsupported. Use a FfxApiUpscaleQualityMode from "
		"FFX_UPSCALE_QUALITY_MODE_NATIVEAA through FFX_UPSCALE_QUALITY_MODE_ULTRA_PERFORMANCE."); }
}

void GDX12FSRUpscalePass::ReleaseContext()
{
	if (_context) { ffxDestroyContext(&_context, nullptr); _context = nullptr; }
}

void GDX12FSRUpscalePass::Check(ffxReturnCode_t result, const char* operation)
{
	if (result != FFX_API_RETURN_OK)
	{ throw std::runtime_error(std::string("ERROR: FSR failed to ") + operation + " (" + std::to_string(result) + "). Check the reported SDK result, FSR context initialization, and device support."); }
}

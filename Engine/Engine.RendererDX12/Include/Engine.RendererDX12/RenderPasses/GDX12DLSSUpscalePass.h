#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include "Engine.RendererDX12/GDX12StreamlineSDK.h"
#include <atomic>

class GDX12DLSSUpscalePass : public GDX12RenderPass
{
public:
	static constexpr const char* Source = "Source";
	static constexpr const char* Depth = "Depth";
	static constexpr const char* Velocity = "Velocity";
	static constexpr const char* Color = "Color";

	explicit GDX12DLSSUpscalePass(sl::DLSSMode quality = sl::DLSSMode::eMaxQuality)
		: _viewportHandle(_nextViewport.fetch_add(1)), _quality(quality)
	{
		if (quality < sl::DLSSMode::eMaxPerformance || quality > sl::DLSSMode::eDLAA)
		{ throw std::invalid_argument("ERROR: Given DLSS quality mode is unsupported. Pass a supported sl::DLSSMode from eMaxPerformance through "
			"eDLAA; eOff is not an upscaling mode."); }
		_flags = RENDER_PASS_FLAG_UPSCALER | RENDER_PASS_FLAG_USE_JITTER | RENDER_PASS_FLAG_USE_STREAMLINE;
	}

	~GDX12DLSSUpscalePass() override
	{ ReleaseFeature(); }

	void Describe(PassDescription& s) const override
	{
		// Streamline is initialized against the primary device by RenderModule.
		s.PrimaryOnly = true;
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

	void PrepareResourceRequirements(PassResourceRequirementsContext& context) override
	{
		auto& sdk = GDX12StreamlineSDK::Get();
		if (!sdk.IsEnabled()) { throw std::runtime_error("ERROR: DLSS requires initialized Streamline. Initialize GDX12StreamlineSDK and register the primary device "
			"before compiling this pass."); }
		LUID luid = context.GetDevice()->GetAdapterLuid();
		sl::AdapterInfo adapter = {};
		adapter.deviceLUID = reinterpret_cast<uint8_t*>(&luid);
		adapter.deviceLUIDSizeInBytes = sizeof(luid);
		Check(sdk.IsFeatureSupported(sl::kFeatureDLSS, adapter), "check device support");
		const auto displaySize = context.GetDisplaySize();
		sl::DLSSOptions options = {};
		options.mode = _quality;
		options.outputWidth = displaySize.x;
		options.outputHeight = displaySize.y;
		sl::DLSSOptimalSettings settings = {};
		Check(sdk.DLSSGetOptimalSettings(options, settings), "query optimal settings");
		const UInt2 renderSize = {settings.optimalRenderWidth, settings.optimalRenderHeight};
		if (!renderSize.x || !renderSize.y) { throw std::runtime_error("ERROR: DLSS returned a zero or invalid input resolution. Use non-zero display dimensions and a supported DLSS "
			"quality mode, then recreate the DLSS context."); }
		context.RequireTextureSize(Source, renderSize);
		context.RequireTextureSize(Depth, renderSize);
		context.RequireTextureSize(Velocity, renderSize);
		_resetHistory = true;
	}

	void Initialize(PassInitContext& context) override
	{ CreateFeature(context.GetOutputResourceDescription(Color).GetSize()); }

	void OnResourcesRecreated(PassInitContext& context) override
	{
		ReleaseFeature();
		CreateFeature(context.GetOutputResourceDescription(Color).GetSize());
		_resetHistory = true;
	}

	void Execute(PassExecutionContext& context) override
	{
		const auto renderSize = context.GetInputResourceDescription(Source).GetSize();
		const auto displaySize = context.GetOutputResourceDescription(Color).GetSize();
		auto* cmdList = context.GetCommandList();
		auto& sdk = GDX12StreamlineSDK::Get();
		sl::FrameToken* frame = nullptr;
		const uint32_t frameIndex = static_cast<uint32_t>(context.GetFrameId());
		Check(sdk.GetNewFrameToken(frame, &frameIndex), "get frame token");
		if (!frame) { throw std::runtime_error("ERROR: DLSS returned a null frame token. Verify Streamline initialization and primary-device registration "
			"before requesting a frame token."); }
		SetConstants(frame, *context.GetFrameData());
		auto* source = context.GetInputTexture(Source).GetTexture();
		auto* depth = context.GetInputTexture(Depth).GetTexture();
		auto* velocity = context.GetInputTexture(Velocity).GetTexture();
		auto* output = context.GetOutputTexture(Color).GetTexture();
		sl::Resource sourceResource{sl::ResourceType::eTex2d, source->GetResource()->D3DResource.Get(),
			nullptr, nullptr, static_cast<uint32_t>(source->GetResource()->GetCurrentState())};
		sl::Resource depthResource{sl::ResourceType::eTex2d, depth->GetResource()->D3DResource.Get(),
			nullptr, nullptr, static_cast<uint32_t>(depth->GetResource()->GetCurrentState())};
		sl::Resource velocityResource{sl::ResourceType::eTex2d, velocity->GetResource()->D3DResource.Get(),
			nullptr, nullptr, static_cast<uint32_t>(velocity->GetResource()->GetCurrentState())};
		sl::Resource outputResource{sl::ResourceType::eTex2d, output->GetResource()->D3DResource.Get(),
			nullptr, nullptr, static_cast<uint32_t>(output->GetResource()->GetCurrentState())};
		sl::Extent inputExtent{};
		inputExtent.width = renderSize.x;
		inputExtent.height = renderSize.y;
		sl::Extent outputExtent{};
		outputExtent.width = displaySize.x;
		outputExtent.height = displaySize.y;
		sl::ResourceTag tags[] = {
			{&sourceResource, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilEvaluate, &inputExtent},
			{&depthResource, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilEvaluate, &inputExtent},
			{&velocityResource, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilEvaluate, &inputExtent},
			{&outputResource, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilEvaluate, &outputExtent}};
		cmdList->BeginPixEvent("DLSS Upscale", Colors::Blue);
		Check(sdk.SetTagForFrame(*frame, _viewportHandle, tags, _countof(tags), cmdList->GetCommandList().Get()), "set resource tags");
		const sl::BaseStructure* inputs[] = {&_viewportHandle};
		Check(sdk.EvaluateFeature(sl::kFeatureDLSS, *frame, inputs, _countof(inputs), cmdList->GetCommandList().Get()), "evaluate");
		cmdList->EndPixEvent();
		_resetHistory = false;
	}

private:
	inline static std::atomic<uint32_t> _nextViewport{0x100000};
	sl::ViewportHandle _viewportHandle;
	const sl::DLSSMode _quality;
	bool _configured = false;
	bool _resetHistory = true;

	void CreateFeature(UInt2 displaySize)
	{
		sl::DLSSOptions options = {};
		options.mode = _quality;
		options.outputWidth = displaySize.x;
		options.outputHeight = displaySize.y;
		options.sharpness = 0.25f;
		options.colorBuffersHDR = sl::Boolean::eFalse;
		Check(GDX12StreamlineSDK::Get().DLSSSetOptions(_viewportHandle, options), "set options");
		_configured = true;
	}

	void ReleaseFeature()
	{
		if (_configured)
		{
			GDX12StreamlineSDK::Get().FreeResources(sl::kFeatureDLSS, _viewportHandle);
			_configured = false;
		}
	}

	static void Check(sl::Result result, const char* operation)
	{
		if (result != sl::Result::eOk)
		{ throw std::runtime_error(std::string("ERROR: DLSS failed to ") + operation + " (" + GDX12StreamlineSDK::ResultToString(result) + "). Check the reported SDK result, Streamline initialization, and DLSS support on the primary adapter."); }
	}

	void SetConstants(sl::FrameToken* currentFrameToken, const RenderPipelineCommonData& common)
	{
		using namespace sl;

		Constants constants = {};

		//expects row major unjittered matrices
		constants.cameraViewToClip = *reinterpret_cast<const sl::float4x4*>(&common.CameraViewToClip);
		constants.clipToCameraView = *reinterpret_cast<const sl::float4x4*>(&common.ClipToCameraView);
		constants.clipToPrevClip = *reinterpret_cast<const sl::float4x4*>(&common.ClipToPrevClip);
		constants.prevClipToClip = *reinterpret_cast<const sl::float4x4*>(&common.PrevClipToClip);

		constants.cameraNear = common.ActiveCameraNearPlane;
		constants.cameraFar = common.ActiveCameraFarPlane;
		constants.cameraFOV = XMConvertToRadians(common.ActiveCameraFOV);
		constants.cameraAspectRatio = static_cast<float>(common.WindowWidth) / common.WindowHeight;

		constants.jitterOffset = { -common.ActiveCameraJitterOffsetX, -common.ActiveCameraJitterOffsetY };
		constants.mvecScale = { 1.f, 1.f };

		constants.depthInverted = Boolean::eTrue;
		constants.cameraMotionIncluded = Boolean::eTrue;
		constants.motionVectors3D = Boolean::eFalse;
		constants.reset = _resetHistory ? Boolean::eTrue : Boolean::eFalse;
		constants.orthographicProjection = Boolean::eFalse;
		constants.motionVectorsDilated = Boolean::eFalse;
		constants.motionVectorsJittered = Boolean::eFalse;

		Result result = GDX12StreamlineSDK::Get().SetConstants(constants, *currentFrameToken, _viewportHandle);
		Check(result, "set constants");
	}
};

#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include "Engine.RendererDX12/GDX12StreamlineSDK.h"
#include <atomic>
#include <cmath>

class GDX12NISUpscalePass : public GDX12RenderPass
{
public:
	static constexpr const char* InColorTexture = "InColorTexture";
	static constexpr const char* OutColorTexture = "OutColorTexture";

	explicit GDX12NISUpscalePass(float upscaleRatio = 1.5f, float sharpness = 0.1f)
		: _viewportHandle(_nextViewport.fetch_add(1)), _defaultUpscaleRatio(upscaleRatio)
	{
		ValidateUpscaleRatio(upscaleRatio);
		SetSharpness(sharpness);
		_flags = RENDER_PASS_FLAG_UPSCALER | RENDER_PASS_FLAG_USE_STREAMLINE;
	}

	void SetSharpness(float sharpness)
	{
		if (!std::isfinite(sharpness) || sharpness < 0.f || sharpness > 1.f)
		{ throw std::invalid_argument("ERROR: Given NIS sharpness is outside [0, 1]. Pass a sharpness value between 0 and 1."); }
		_sharpness = sharpness;
	}
	float GetSharpness() const { return _sharpness; }

	~GDX12NISUpscalePass() override
	{ ReleaseFeature(); }

	void Describe(PassDescription& s) const override
	{
		s.AddParameter("UpscaleRatio", _defaultUpscaleRatio);
		s.AddParameter("Sharpness", _sharpness);
		// Streamline is initialized against the primary device by RenderModule.
		s.PrimaryOnly = true;
		auto& source = s.AddInputTexture(InColorTexture);
		source.Formats = {DXGI_FORMAT_R8G8B8A8_UNORM};
		source.Usages = ResourceUsage::NonPixelShaderRead;
		auto& color = s.AddOutputTexture(OutColorTexture);
		color.Formats = {DXGI_FORMAT_R8G8B8A8_UNORM};
		color.Size = Extent::Display();
		color.Usages = ResourceUsage::UnorderedAccess;
	}

	void ResolveResourceRequirements(PassResourceRequirementsContext& context) override
	{
		const auto upscaleRatio = context.GetParameter<float>("UpscaleRatio");
		ValidateUpscaleRatio(upscaleRatio);
		auto& sdk = GDX12StreamlineSDK::Get();
		if (!sdk.IsEnabled()) { throw std::runtime_error("ERROR: NIS requires initialized Streamline. Initialize GDX12StreamlineSDK and register the primary device "
			"before compiling this pass."); }
		LUID luid = context.GetDevice()->GetAdapterLuid();
		sl::AdapterInfo adapter = {};
		adapter.deviceLUID = reinterpret_cast<uint8_t*>(&luid);
		adapter.deviceLUIDSizeInBytes = sizeof(luid);
		Check(sdk.IsFeatureSupported(sl::kFeatureNIS, adapter), "check device support");
		const auto displaySize = context.GetDisplaySize();
		const UInt2 renderSize = {static_cast<uint32_t>(displaySize.x / upscaleRatio), static_cast<uint32_t>(displaySize.y / upscaleRatio)};
		if (!renderSize.x || !renderSize.y) { throw std::runtime_error("ERROR: NIS input resolution is zero. Use non-zero display dimensions and an upscale ratio that produces a "
			"non-zero input size."); }
		context.RequireTextureSize(InColorTexture, renderSize);
	}

	void Initialize(PassInitContext& context) override
	{
		SetSharpness(context.GetParameter<float>("Sharpness"));
		SetOptions(_sharpness);
	}

	void OnResourcesRecreated(PassInitContext&) override
	{
		ReleaseFeature();
		SetOptions(_sharpness);
	}

	void Execute(PassExecutionContext& context) override
	{
		const auto renderSize = context.GetInputResourceDescription(InColorTexture).GetSize();
		const auto displaySize = context.GetOutputResourceDescription(OutColorTexture).GetSize();
		auto* cmdList = context.GetCommandList();
		auto& sdk = GDX12StreamlineSDK::Get();
		if (_sharpness != _appliedSharpness) { SetOptions(_sharpness); }
		sl::FrameToken* frame = nullptr;
		const uint32_t frameIndex = static_cast<uint32_t>(context.GetFrameId());
		Check(sdk.GetNewFrameToken(frame, &frameIndex), "get frame token");
		if (!frame) { throw std::runtime_error("ERROR: NIS returned a null frame token. Verify Streamline initialization and primary-device registration "
			"before requesting a frame token."); }
		auto* source = context.GetInputTexture(InColorTexture).GetTexture();
		auto* output = context.GetOutputTexture(OutColorTexture).GetTexture();
		sl::Resource sourceResource{sl::ResourceType::eTex2d, source->GetResource()->D3DResource.Get(),
			nullptr, nullptr, static_cast<uint32_t>(source->GetResource()->GetCurrentState())};
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
			{&outputResource, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilEvaluate, &outputExtent}};
		cmdList->BeginPixEvent("NIS Upscale", Colors::Blue);
		Check(sdk.SetTagForFrame(*frame, _viewportHandle, tags, _countof(tags), cmdList->GetCommandList().Get()), "set resource tags");
		const sl::BaseStructure* inputs[] = {&_viewportHandle};
		Check(sdk.EvaluateFeature(sl::kFeatureNIS, *frame, inputs, _countof(inputs), cmdList->GetCommandList().Get()), "evaluate");
		cmdList->EndPixEvent();
	}

private:
	inline static std::atomic<uint32_t> _nextViewport{0x200000};
	sl::ViewportHandle _viewportHandle;
	const float _defaultUpscaleRatio;
	float _sharpness = 0.1f;
	float _appliedSharpness = -1.f;
	bool _configured = false;

	static void ValidateUpscaleRatio(float upscaleRatio)
	{
		if (!std::isfinite(upscaleRatio) || upscaleRatio < 1.f || upscaleRatio > 3.f)
		{ throw std::invalid_argument("ERROR: Given NIS upscale ratio is outside [1, 3]. Use an upscale ratio between 1 and 3."); }
	}

	void SetOptions(float sharpness)
	{
		sl::NISOptions options = {};
		options.mode = sl::NISMode::eScaler;
		options.hdrMode = sl::NISHDR::eNone;
		options.sharpness = sharpness;
		Check(GDX12StreamlineSDK::Get().NISSetOptions(_viewportHandle, options), "set options");
		_appliedSharpness = sharpness;
		_configured = true;
	}

	void ReleaseFeature()
	{
		if (_configured)
		{
			GDX12StreamlineSDK::Get().FreeResources(sl::kFeatureNIS, _viewportHandle);
			_configured = false;
		}
	}

	static void Check(sl::Result result, const char* operation)
	{
		if (result != sl::Result::eOk)
		{ throw std::runtime_error(std::string("ERROR: NIS failed to ") + operation + " (" + GDX12StreamlineSDK::ResultToString(result) + "). Check the reported SDK result, Streamline initialization, and NIS support on the primary adapter."); }
	}
};

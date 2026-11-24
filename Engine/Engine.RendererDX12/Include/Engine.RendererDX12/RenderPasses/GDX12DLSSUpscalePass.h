#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include "Engine.RendererDX12/GDX12StreamlineSDK.h"
#include <atomic>

class GDX12DLSSUpscalePass : public GDX12RenderPass
{
public:
	static constexpr const char* InColorTexture = "InColorTexture";
	static constexpr const char* InDepthBuffer = "InDepthBuffer";
	static constexpr const char* InVelocityBuffer = "InVelocityBuffer";
	static constexpr const char* InTransparencyMask = "InTransparencyMask";
	static constexpr const char* OutColorTexture = "OutColorTexture";

	explicit GDX12DLSSUpscalePass(sl::DLSSMode quality = sl::DLSSMode::eMaxQuality);
	~GDX12DLSSUpscalePass() override;
	void Describe(PassDescription& s) const override;
	void ResolveResourceRequirements(PassResourceRequirementsContext& context) override;
	void Initialize(PassInitContext& context) override;
	void OnResourcesRecreated(PassInitContext& context) override;
	void Execute(PassExecutionContext& context) override;

private:
	inline static std::atomic<uint32_t> _nextViewport{0x100000};
	sl::ViewportHandle _viewportHandle;
	sl::DLSSMode _quality;
	bool _configured = false;
	bool _resetHistory = true;

	static void ValidateQuality(sl::DLSSMode quality);
	void CreateFeature(UInt2 displaySize);
	void ReleaseFeature();
	static void Check(sl::Result result, const char* operation);
	void SetConstants(sl::FrameToken* currentFrameToken, const RenderPipelineCommonData& common);
};

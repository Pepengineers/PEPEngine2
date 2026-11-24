#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include "ffx-api/ffx_api.h"
#include "ffx-api/ffx_upscale.h"

class GDX12FSRUpscalePass : public GDX12RenderPass
{
public:
	static constexpr const char* InColorTexture = "InColorTexture";
	static constexpr const char* InDepthBuffer = "InDepthBuffer";
	static constexpr const char* InVelocityBuffer = "InVelocityBuffer";
	static constexpr const char* InTransparencyMask = "InTransparencyMask";
	static constexpr const char* OutColorTexture = "OutColorTexture";

	explicit GDX12FSRUpscalePass(FfxApiUpscaleQualityMode quality = FFX_UPSCALE_QUALITY_MODE_QUALITY);
	~GDX12FSRUpscalePass() override;
	void Describe(PassDescription& s) const override;
	void ResolveResourceRequirements(PassResourceRequirementsContext& context) override;
	void Execute(PassExecutionContext& context) override;
	void OnResourcesRecreated(PassInitContext&) override;

private:
	const FfxApiUpscaleQualityMode _defaultQuality;
	ffxContext _context = nullptr;
	bool _resetHistory = true;
	bool _enableSharpening = true;
	float _sharpness = 1.f;

	static void ValidateQuality(FfxApiUpscaleQualityMode quality);
	void ReleaseContext();
	static void Check(ffxReturnCode_t result, const char* operation);
};

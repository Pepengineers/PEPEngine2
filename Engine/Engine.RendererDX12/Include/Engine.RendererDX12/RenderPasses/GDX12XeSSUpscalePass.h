#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include "xess/xess.h"

class GDX12XeSSUpscalePass : public GDX12RenderPass
{
public:
	static constexpr const char* InColorTexture = "InColorTexture";
	static constexpr const char* InDepthBuffer = "InDepthBuffer";
	static constexpr const char* InVelocityBuffer = "InVelocityBuffer";
	static constexpr const char* InTransparencyMask = "InTransparencyMask";
	static constexpr const char* OutColorTexture = "OutColorTexture";

	explicit GDX12XeSSUpscalePass(xess_quality_settings_t quality = XESS_QUALITY_SETTING_ULTRA_QUALITY_PLUS);
	~GDX12XeSSUpscalePass() override;
	void Describe(PassDescription& s) const override;
	void ResolveResourceRequirements(PassResourceRequirementsContext& context) override;
	void Execute(PassExecutionContext& context) override;
	void OnResourcesRecreated(PassInitContext&) override;

private:
	xess_context_handle_t _context = nullptr;
	const xess_quality_settings_t _defaultQuality;
	bool _resetHistory = true;

	static void ValidateQuality(xess_quality_settings_t quality);
	void ReleaseContext();
	void Check(xess_result_t result, const char* operation);
	std::string XeSSResultToString(const xess_result_t result);
};

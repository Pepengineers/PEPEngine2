#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include "Engine.RendererDX12/GDX12StreamlineSDK.h"
#include <atomic>

class GDX12NISUpscalePass : public GDX12RenderPass
{
public:
	static constexpr const char* InColorTexture = "InColorTexture";
	static constexpr const char* OutColorTexture = "OutColorTexture";

	explicit GDX12NISUpscalePass(float upscaleRatio = 1.5f, float sharpness = 0.1f);
	void SetSharpness(float sharpness);
	float GetSharpness() const;
	~GDX12NISUpscalePass() override;
	void Describe(PassDescription& s) const override;
	void ResolveResourceRequirements(PassResourceRequirementsContext& context) override;
	void Initialize(PassInitContext& context) override;
	void OnResourcesRecreated(PassInitContext&) override;
	void Execute(PassExecutionContext& context) override;

private:
	inline static std::atomic<uint32_t> _nextViewport{0x200000};
	sl::ViewportHandle _viewportHandle;
	const float _defaultUpscaleRatio;
	float _sharpness = 0.1f;
	float _appliedSharpness = -1.f;
	bool _configured = false;

	static void ValidateUpscaleRatio(float upscaleRatio);
	void SetOptions(float sharpness);
	void ReleaseFeature();
	static void Check(sl::Result result, const char* operation);
};

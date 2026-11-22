#pragma once
#include <chrono>
#include "App.Base/Rendering/IRenderPipeline.h"
#include "Techniques/SceneGeometry.h"
#include "Techniques/SceneEnvironment.h"
#include "Techniques/SceneLighting.h"
#include "Techniques/UI/ScenePostUI.h"
#include "Techniques/SceneCloudPicture.h"
#include "Common/GameTimer.h"
#include <optional>

namespace BistroWaves { class Controller; }

class BistroRenderPipeline final : public IRenderPipeline
{
  public:
	void Initialize(RenderPipelineContext&, const std::vector<DeviceDesc>&) override;
	void PrepareMaterialBindings(RenderPipelineContext&) override;
	void DrainExecution() override
	{
		if (_lighting) _lighting->DrainLightGrid();
		if (_post) { _post->DrainHud(); _post->DrainPost(); }
		if (_executor)
		{
			_executor->Drain();
			_geometry->DrainSkinning();
		}
		if (_post)
		{
			_post->ReleasePresentation();
		}
	}
	void UpdateFrameSettings(RenderPipelineContext&) override;
	void ResetWorldHistory() override
	{
		if (_post)
		{
			_post->Reset();
		}
		if (_environment)
		{
			_environment->ResetClouds();
		}
		if (_lighting)
		{
			_lighting->Reset();
		}
	}
	std::pair<size_t, size_t> GetWorldProbeReadiness() const override
	{
		return _lighting ? _lighting->Readiness() : std::pair<size_t, size_t>{};
	}
	void PrepareFrame(RenderPipelineContext&) override;
	void RebuildExecution(RenderPipelineContext&) override;
	void Resize(RenderPipelineContext&) override
	{
		DrainExecution();
		_post->Resize();
	}
	const RenderPipelineBindings& GetBindings() const override { return _bindings; }
	uint64_t ExecuteFrame(RenderPipelineContext&, GDX12CommandList*,
	                      const std::function<void(GDX12CommandList*)>&) override;
	void FramePresented() override;
	void BeforePresent() override { _beforePresent = std::chrono::steady_clock::now(); }
	auto PresentTime() const { return _beforePresent; }
	void WriteExecution(std::ostream& output) const override { _schedule->Write(output); }
	const BistroRender::Schedule& LastSchedule() const { return *_schedule; }
	struct FrameLink { BistroRender::Id Export, Owner, Import; };
	void CheckPanelPlacement(uint64_t revision) { _checkPanelPlacement = true; _panelSettings = revision; }
	const auto& PanelSchedule() const { return _panelSchedule; }
	const auto& PanelConsumers() const { return _panelConsumers; }
	const auto& PanelOwners() const { return _panelOwners; }
	uint64_t PanelRevision() const { return _panelRevision; }
	const auto& FrameLinks() const { return _frameLinks; }
	const auto& BenchmarkLinks() const { return _benchmarkLinks; }
	const auto& BenchmarkSchedule() const { return _schedule; }
	SkySource BenchmarkSkySource() const { return _benchmarkSky; }
	auto BenchmarkPostPublication() const { return _post->BenchmarkPublication(); }
	void AppendBenchmarkProbes(std::vector<std::pair<std::string, BistroRender::Id>>& owners) const
	{
		_lighting->AppendPublishedProbes(owners);
	}
	void AppendBenchmarkShadowOwners(std::vector<std::pair<UINT, BistroRender::Id>>& owners) const
	{
		_lighting->AppendBenchmarkShadowOwners(owners);
	}
	std::vector<std::pair<std::string, std::string>> PanelAdapters() const;
	uint64_t PresentedFrames() const { return _presentedFrames; }
	void SetBenchmarkTime(std::optional<double> seconds, bool localSkinning = true)
	{
		_benchmarkTime = seconds;
		_benchmarkLocalSkinning = seconds.has_value() && localSkinning;
	}
	void ResetBenchmarkClouds() { _environment->ResetClouds(); }
	std::function<void()> CaptureBenchmarkPhysics(World&);
	std::vector<std::pair<std::string, std::string>> BenchmarkAdapters() const;
	const auto& BenchmarkSubmissions() const { return *_executor->Submissions(); }
	auto& BenchmarkSubmissions() { return *_executor->Submissions(); }
	auto BenchmarkTransferBytes() const { return _post->BenchmarkTransferBytes(); }
	auto BenchmarkPostEffects() const { return _post->BenchmarkPostEffects(); }
	void PauseIndependentClocks();

  private:
	bool _checkPanelPlacement = false;
	uint64_t _panelSettings = 0, _capturedSettings = 0;
	uint64_t _panelRevision = 0;
	std::shared_ptr<const BistroRender::Schedule> _panelSchedule;
	std::vector<std::vector<BistroRender::Id>> _panelConsumers;
	std::vector<std::pair<std::string, std::string>> _panelOwners;
	std::vector<FrameLink> _frameLinks;
	std::vector<FrameLink> _benchmarkLinks;
	SkySource _benchmarkSky = SkySource::Off;
	RenderPipelineBindings _bindings;
	std::unique_ptr<BistroRender::SceneGeometry> _geometry;
	std::unique_ptr<BistroRender::SceneEnvironment> _environment;
	std::unique_ptr<BistroRender::SceneLighting> _lighting;
	GDX12RenderPass _sceneFlags;
	std::unique_ptr<BistroRender::ScenePostUI> _post;
	std::unique_ptr<BistroRender::SceneCloudPicture> _cloudPicture;
	std::unique_ptr<BistroRender::Executor> _executor;
	std::shared_ptr<const BistroRender::Schedule> _schedule = std::make_shared<BistroRender::Schedule>();
	double _animationTime = 0;
	std::optional<double> _benchmarkTime;
	bool _benchmarkLocalSkinning = false;
	std::shared_ptr<BistroWaves::Controller> _waves;
	uint64_t _presentedFrames = 0;
	std::chrono::steady_clock::time_point _beforePresent{};
	uint64_t _frameNumber = 0;
	std::array<BistroRender::Id, 5> _reportedDevices{
	    BistroRender::Invalid, BistroRender::Invalid, BistroRender::Invalid, BistroRender::Invalid, BistroRender::Invalid};
	int _aoAdapter = -1;
	BistroRender::Id _transparencyOwner = 0;
	UINT _executorSlot = 0;
	bool _reportedDepthNormals = false;
	bool _reportedLocalAO = false;
};

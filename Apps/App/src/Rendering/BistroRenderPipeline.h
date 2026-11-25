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
	                      const std::function<void(GDX12CommandList*)>&, PrefixContents = PrefixContents::Opaque) override;
	void FramePresented() override;
	void BeforePresent() override { _beforePresent = std::chrono::steady_clock::now(); }
	auto PresentTime() const { return _beforePresent; }
	auto PresentCompletedAt() const { return _afterPresent; }
	void WriteExecution(std::ostream& output) const override { _schedule->Write(output); }
	const BistroRender::Schedule& LastSchedule() const { return *_schedule; }
	bool CloudPictureSizeUnavailable() const { return _cloudPicture->SizeUnavailable(); }
	bool HasRemoteCloudPicture() const { return _cloudPicture->IsRemote(); }
	struct FrameLink { BistroRender::Id Owner, Import; };
	const auto& BenchmarkLinks() const { return _benchmarkLinks; }
	struct PacketRoles { bool AO = false, LightGrid = false, Particles = false, Water = false, Wind = false; };
	using Owners = std::vector<std::pair<std::string, BistroRender::Id>>;
	struct FramePlacement
	{
		Owners ActiveOwners;
		std::pair<BistroRender::Id, BistroRender::Id> PostPublication;
		UINT64 ShadowTexels = 0;
	};
	void BenchmarkPlacement(const PacketRoles&, FramePlacement&) const;
	UINT BenchmarkSlot() const { return _executorSlot; }
	const auto& BenchmarkSchedule() const { return _schedule; }
	SkySource BenchmarkSkySource() const { return _benchmarkSky; }
	bool BenchmarkLocalCloudsActive() const { return _environment->LocalCloudsActive(); }
	const auto& BenchmarkProbeFrame() const { return _lighting->BenchmarkProbeFrame(); }
	void ResetBenchmarkProbeCaptures() { _lighting->ResetBenchmarkProbeCaptures(); }
	BistroRender::CompletionToken BenchmarkFrameFence() const { return _executor->Completions().back(); }
	bool BenchmarkPublicationsReady() const
	{
		return _lighting->Water().BenchmarkReady() && _lighting->Particles().BenchmarkReady() && _environment->BenchmarkReady();
	}
	const auto& BenchmarkSimulations() const { return _benchmarkSimulations; }
	const auto& BenchmarkNISFailure() const { return _post->BenchmarkNISFailure(); }
	std::vector<std::pair<std::string, std::string>> PanelAdapters() const;
	uint64_t PresentedFrames() const { return _presentedFrames; }
	void SetBenchmarkTime(std::optional<double> routeTime, double warmup = 0, bool localSkinning = true, bool observePlacement = false)
	{
		_benchmarkTime = routeTime;
		_benchmarkWarmup = warmup;
		_observePlacement = observePlacement;
		_benchmarkLocalSkinning = routeTime.has_value() && localSkinning;
	}
	void ResetBenchmarkClouds() { _environment->ResetClouds(); }
	void RetireLocalShadows()
	{
		if (!_lighting || !_executor) return;
		_executor->Drain();
		_lighting->RetireLocalShadows();
	}
	std::function<void()> CaptureBenchmarkPhysics(World&);
	std::vector<std::pair<std::string, std::string>> BenchmarkAdapters() const;
	const auto& BenchmarkSubmissions() const { return *_executor->Submissions(); }
	auto& BenchmarkSubmissions() { return *_executor->Submissions(); }
	auto BenchmarkTransferBytes() const { return _post->BenchmarkTransferBytes(); }
	auto BenchmarkPostEffects() const { return _post->BenchmarkPostEffects(); }
	void PauseIndependentClocks();

  private:
	bool _observePlacement = false;
	std::vector<FrameLink> _benchmarkLinks;
	std::vector<BistroRender::SimulationPublication> _benchmarkSimulations;
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
	double _benchmarkWarmup = 0;
	bool _benchmarkLocalSkinning = false;
	std::shared_ptr<BistroWaves::Controller> _waves;
	uint64_t _presentedFrames = 0;
	std::chrono::steady_clock::time_point _beforePresent{};
	std::chrono::steady_clock::time_point _afterPresent{};
	uint64_t _frameNumber = 0;
	std::array<BistroRender::Id, 5> _reportedDevices{
	    BistroRender::Invalid, BistroRender::Invalid, BistroRender::Invalid, BistroRender::Invalid, BistroRender::Invalid};
	int _aoAdapter = -1;
	BistroRender::Id _transparencyOwner = 0;
	UINT _executorSlot = 0;
	bool _reportedDepthNormals = false;
	bool _reportedLocalAO = false;
};

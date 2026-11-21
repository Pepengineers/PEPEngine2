#pragma once
#include "App.Base/Rendering/IRenderPipeline.h"
#include "Techniques/SceneGeometry.h"
#include "Techniques/SceneEnvironment.h"
#include "Techniques/SceneLighting.h"
#include "Techniques/UI/ScenePostUI.h"
#include "Techniques/SceneCloudPicture.h"
#include "Common/GameTimer.h"

class BistroRenderPipeline final : public IRenderPipeline
{
  public:
	void Initialize(RenderPipelineContext&) override;
	void PrepareMaterialBindings(RenderPipelineContext&) override;
	void DrainExecution() override
	{
		if (_executor)
		{
			_executor->Drain();
		}
		if (_post)
		{
			_post->ReleasePresentation();
		}
	}
	void UpdateFrameSettings(RenderPipelineContext& c) override
	{
		if (!(GetReconstructionSettings() == _post->Reconstruction().GetStatus().Requested) || _post->NeedsResize())
		{
			DrainExecution();
		}
		if (_post->Reconstruction().ApplySettings())
		{
			c.Resize();
		}
	}
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
		_executor->Drain();
		_post->Resize();
	}
	const RenderPipelineBindings& GetBindings() const override { return _bindings; }
	uint64_t ExecuteFrame(RenderPipelineContext&, GDX12CommandList*,
	                      const std::function<void(GDX12CommandList*)>&) override;
	void FramePresented() override { _cloudPicture->Presented(); }
	void WriteExecution(std::ostream& output) const override { _schedule.Write(output); }
	const BistroRender::Schedule& LastSchedule() const { return _schedule; }
	bool HasRemoteCloudPicture() const { return _cloudPicture->IsRemote(); }

  private:
	RenderPipelineBindings _bindings;
	std::unique_ptr<BistroRender::SceneGeometry> _geometry;
	std::unique_ptr<BistroRender::SceneEnvironment> _environment;
	std::unique_ptr<BistroRender::SceneLighting> _lighting;
	GDX12RenderPass _sceneFlags;
	std::unique_ptr<BistroRender::ScenePostUI> _post;
	std::unique_ptr<BistroRender::SceneCloudPicture> _cloudPicture;
	std::unique_ptr<BistroRender::Executor> _executor;
	BistroRender::Schedule _schedule;
	double _animationTime = 0;
};

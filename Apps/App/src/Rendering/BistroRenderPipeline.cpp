#include "BistroRenderPipeline.h"
#include "Engine.RendererDX12/GDX12SwapChain.h"
#include "Engine.Core/BenchmarkEngine.h"
#include "Engine.Core/Logging/Logger.h"
#include "App.Base/Modules/RenderModule.h"
#include "App.Base/Modules/SceneManagerModule.h"
#include "Common/ConsoleVariables.h"
#include "BistroScene.h"

namespace
{
AutoConsoleVariable<bool> AsyncQueues(L"Meta.Async.Queues", true,
                                      L"Place explicit local work on COMPUTE/COPY; false uses DIRECT.",
                                      EConsoleVariableFlags::Default);
}

void BistroRenderPipeline::Initialize(RenderPipelineContext &c)
{
	c.Primary.SRV_UAV_Heap->UseTransientDescriptorTail();
	_post = std::make_unique<Meta::ScenePostUI>(c);
	ConfigureBistroProps(*BenchmarkEngine::GetLocator().GetModule<SceneManagerModule>(), App::GetInstance()->GetJobs());
	_cloudPicture = std::make_unique<Meta::SceneCloudPicture>(c);
	_geometry = std::make_unique<Meta::SceneGeometry>(c);
	_environment = std::make_unique<Meta::SceneEnvironment>(c.Primary, c.Common);
	_lighting = std::make_unique<Meta::SceneLighting>(c.Primary, c.Common);
	_sceneFlags.SetFlag(RENDER_PASS_FLAG_USE_CAMERAS, true);
	_sceneFlags.SetFlag(RENDER_PASS_FLAG_USE_INSTANCES, true);
	_sceneFlags.SetFlag(RENDER_PASS_FLAG_USE_GEOMETRY, true);
	_sceneFlags.SetFlag(RENDER_PASS_FLAG_USE_MATERIALS, true);
	_bindings.PrimaryPasses = _post->Passes();
	_bindings.PrimaryPasses.push_back(&_sceneFlags);
	_bindings.Reconstruction = &_post->Reconstruction();
	_executor =
	    std::make_unique<Meta::Executor>(_cloudPicture->Submissions(), 0, UINT(c.Primary.FrameConstants.size()));
	_lighting->SetWaterRetirement([this] { _executor->Drain(); });
	_environment->SetRetirement([this] { _executor->Drain(); });
	Log::Info("Renderer", "Bistro packet renderer initialized; one rendering path");
}

void BistroRenderPipeline::RebuildExecution(RenderPipelineContext &c)
{
	_executor->Drain();
	_geometry->Resize(c.Common.DownscaledWidth, c.Common.DownscaledHeight);
	_environment->Resize();
}

void BistroRenderPipeline::PrepareFrame(RenderPipelineContext &c)
{
	if (c.Primary.Sky.Source == SkySource::SkyAtmosphere)
	{
		c.Primary.Lighting.SunShadowFlags &= ~2u;
	}
	c.Common.AOAfterDepthPrepass = false;
	_geometry->PrepareGrass(c, c.Common.GameTimer->DeltaTime());
	c.Runtime.PrepareReflectionScene();
}

uint64_t BistroRenderPipeline::ExecuteFrame(RenderPipelineContext &c, GDX12CommandList *prefix,
                                            const std::function<void(GDX12CommandList *)> &finish)
{
	const auto slot = c.Primary.CurrFrameConstantsIndex;
	_geometry->Freeze(slot, c.Common.ActiveCameraCBufferIndex, c.Runtime.FreezeSkeletalMeshes());
	const auto scenes = BenchmarkEngine::GetLocator().GetModule<SceneManagerModule>();
	const auto *world = scenes->GetWorld();
	const double delta = c.Common.GameTimer ? c.Common.GameTimer->DeltaTime() : 0;
	_environment->Freeze(world && !world->IsPaused() ? delta * world->GetTimeScale() : 0);
	_lighting->Freeze(*_geometry, slot);
	auto *target = c.BackBuffer->GetCurrentBuffer();
	const auto append = [&](Meta::Graph &graph, std::vector<ID3D12Resource *> &resources)
	{
		_post->BeginPlan();
		_environment->Append(graph, resources);
		_lighting->SetHDRContinuation(
		    [&](auto &g, auto &bindings, auto *hdr)
		    {
			    _post->Append(g, bindings, hdr, _lighting->PostDepth(), _lighting->PostMotion(),
			                  _lighting->ParticleMotion(), target, finish);
		    });
		_lighting->Append(graph, resources, *_geometry, *_environment);
	};
	const auto prelude = [&](Meta::Graph &graph, std::vector<ID3D12Resource *> &resources)
	{
		_cloudPicture->Append(graph, resources);
		_post->AppendPosters(graph, resources);
	};
	_schedule = _geometry->Plan(slot, append, prelude, !AsyncQueues.GetValue());
	_executor->Reserve(_schedule);
	const auto record = [&](Meta::Id op, uint32_t list, ID3D12GraphicsCommandList *commands)
	{
		if (!_cloudPicture->Record(op, commands) && !_environment->Record(op, commands) &&
		    !_lighting->Record(op, commands) && !_post->Record(op, commands))
		{
			_geometry->Record(slot, op, list, commands);
		}
	};
	uint64_t completion;
	try
	{
		completion = _executor->Execute(_schedule, _geometry->PlannedResources(slot), record, prefix, nullptr, nullptr,
		                                _cloudPicture->Waits(_schedule));
		_cloudPicture->RetireFrame(_schedule, _executor->Completions(), true);
	}
	catch (...)
	{
		_post->Reconstruction().CancelRecording();
		try
		{
			_cloudPicture->RetireFrame(_schedule, _executor->Completions(), false);
		}
		catch (...)
		{
		}
		throw;
	}
	_post->Submitted(slot);
	_environment->Submitted(completion);
	_lighting->Submitted(completion);
	return completion;
}

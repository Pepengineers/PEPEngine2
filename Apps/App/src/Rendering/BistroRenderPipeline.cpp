#include "BistroRenderPipeline.h"
#include "Engine.RendererDX12/GDX12SwapChain.h"
#include "Engine.Core/BenchmarkEngine.h"
#include "Engine.Core/Logging/Logger.h"
#include "Engine.UI/UI_test.h"
#include "App.Base/Modules/RenderModule.h"
#include "App.Base/Modules/SceneManagerModule.h"
#include "Common/ConsoleVariables.h"
#include "BistroScene.h"

namespace
{
AutoConsoleVariable<bool> AsyncQueues(L"App.Async.Queues", true,
                                      L"Place explicit local work on COMPUTE/COPY; false uses DIRECT.",
                                      EConsoleVariableFlags::Default);

}

void BistroRenderPipeline::Initialize(RenderPipelineContext& c)
{
	c.Primary.SRV_UAV_Heap->UseTransientDescriptorTail();
	_post = std::make_unique<BistroRender::ScenePostUI>(c);
	ConfigureBistroProps(*BenchmarkEngine::GetLocator().GetModule<SceneManagerModule>(), App::GetInstance()->GetJobs());
	_cloudPicture = std::make_unique<BistroRender::SceneCloudPicture>(c);
	_geometry = std::make_unique<BistroRender::SceneGeometry>(c);
	_environment = std::make_unique<BistroRender::SceneEnvironment>(c.Primary, c.Common);
	_lighting = std::make_unique<BistroRender::SceneLighting>(c.Primary, c.Common);
	_sceneFlags.SetFlag(RENDER_PASS_FLAG_USE_CAMERAS, true);
	_sceneFlags.SetFlag(RENDER_PASS_FLAG_USE_INSTANCES, true);
	_sceneFlags.SetFlag(RENDER_PASS_FLAG_USE_GEOMETRY, true);
	_sceneFlags.SetFlag(RENDER_PASS_FLAG_USE_MATERIALS, true);
	_bindings.PrimaryPasses = _post->Passes();
	_bindings.PrimaryPasses.push_back(&_sceneFlags);
	_bindings.Reconstruction = &_post->Reconstruction();
	_executor = std::make_unique<BistroRender::Executor>(*c.Primary.Device, UINT(c.Primary.FrameConstants.size()));
	_lighting->SetWaterRetirement([this] { _executor->Drain(); });
	_environment->SetRetirement([this] { _executor->Drain(); });
	Log::Info("Renderer", "Bistro packet renderer initialized; one rendering path");
}

void BistroRenderPipeline::PrepareMaterialBindings(RenderPipelineContext& c)
{
	_cloudPicture->UpdateSettings([this] { _executor->Drain(); });
	const UINT slot = c.Primary.CurrFrameConstantsIndex;
	_executor->Begin(slot);
	_animationTime += (std::max)(0.0, double(c.Common.GameTimer->DeltaTime()));
	_cloudPicture->Prepare(slot, _animationTime);
	_post->PrepareMaterials(_animationTime, _cloudPicture->Image());
}

void BistroRenderPipeline::RebuildExecution(RenderPipelineContext& c)
{
	_executor->Drain();
	_geometry->Resize(c.Common.DownscaledWidth, c.Common.DownscaledHeight);
	_environment->Resize();
}

void BistroRenderPipeline::PrepareFrame(RenderPipelineContext& c)
{
	if (c.Primary.Sky.Source == SkySource::SkyAtmosphere)
	{
		c.Primary.Lighting.SunShadowFlags &= ~2u;
	}
	c.Common.AOAfterDepthPrepass = false;
	_geometry->PrepareGrass(c, c.Common.GameTimer->DeltaTime());
	c.Runtime.PrepareReflectionScene();
}

uint64_t BistroRenderPipeline::ExecuteFrame(RenderPipelineContext& c, GDX12CommandList* prefix,
                                            const std::function<void(GDX12CommandList*)>& finish)
{
	const auto slot = c.Primary.CurrFrameConstantsIndex;
	_geometry->Freeze(slot, c.Common.ActiveCameraCBufferIndex, c.Runtime.FreezeSkeletalMeshes());
	const auto scenes = BenchmarkEngine::GetLocator().GetModule<SceneManagerModule>();
	const auto* world = scenes->GetWorld();
	const double delta = c.Common.GameTimer ? c.Common.GameTimer->DeltaTime() : 0;
	_environment->Freeze(world && !world->IsPaused() ? delta * world->GetTimeScale() : 0);
	_lighting->Freeze(*_geometry, slot);
	auto& particles = _lighting->Particles();
	auto& water = _lighting->Water();
	using ParticleInputs = BistroRender::SceneParticles::Inputs;
	auto* depth = _geometry->Depth(slot);
	particles.BeginSimulation(_geometry->InteractionField());
	water.Prepare(float(delta));
	water.SetSceneInputs(depth, _geometry->GBuffer(slot, 1));
	auto* target = c.BackBuffer->GetCurrentBuffer();
	const bool singleDirect = !AsyncQueues.GetValue();
	const bool earlyEnvironment = !singleDirect && _environment->AsyncComputeEnabled();
	const auto append = [&](BistroRender::Graph& graph, std::vector<ID3D12Resource*>& resources)
	{
		_post->BeginPlan();
		using View = BistroRender::SceneTextureInspector::View;
		const bool inspect = Engine::UI::DebugUIVisible();
		auto& textures = _post->Textures();
		if (inspect)
		{
			textures.Name("Scene.Depth", depth, View::Depth);
			textures.Name("Scene.AO", _lighting->AOTexture(slot), View::Scalar);
			textures.Name("GBuffer.AlbedoMetallic", _geometry->GBuffer(slot, 0));
			textures.Name("GBuffer.Motion", _geometry->GBuffer(slot, 1), View::Motion);
			textures.Name("GBuffer.NormalRoughness", _geometry->GBuffer(slot, 2), View::Normals);
			textures.Name("GBuffer.F0Baked", _geometry->GBuffer(slot, 3));
			textures.Name("GBuffer.Emissive", _geometry->GBuffer(slot, 4), View::HDR);
			textures.Name("Environment.CloudPicture", _cloudPicture->Image());
		}
		particles.AppendOpaque(graph, resources, *_geometry, slot);
		if (!earlyEnvironment)
		{
			_environment->Append(graph, resources);
		}
		const auto posters = _post->AppendPosters(graph, resources);
		_geometry->AppendWorldCanvases(graph, slot, posters);
		_lighting->SetHDRContinuation(
		    [&, inspect](auto& g, auto& bindings, auto* hdr)
		    {
			    if (inspect)
			    {
				    textures.Name("Lighting.DeferredHDR", _lighting->DeferredHDR(), View::HDR);
				    textures.Name("Lighting.SunShadowAtlas", _lighting->SunShadow(slot), View::Scalar);
				    textures.Name("Environment.BRDF", _environment->Output(2)->GetTexture());
				    textures.Name("Scene.HDR", hdr, View::HDR);
				    textures.Name("Post.Depth", _lighting->PostDepth(), View::Depth);
				    textures.Name("Post.Motion", _lighting->PostMotion(), View::Motion);
			    }
			    _post->Append(g, bindings, hdr, _lighting->PostDepth(), _lighting->PostMotion(),
			                  _lighting->ParticleMotion(), target, finish);
		    });
		_lighting->Append(graph, resources, *_geometry, *_environment);
		for (auto& op : graph.Operations)
		{
			if (op.Name == BistroRender::ProbeColorOperationName)
			{
				op.Accesses.insert(op.Accesses.end(), posters.begin(), posters.end());
			}
		}
	};
	const auto prelude = [&](BistroRender::Graph& graph, std::vector<ID3D12Resource*>& resources)
	{
		_cloudPicture->Append(graph, resources);
		particles.AppendSimulation(graph, resources, ParticleInputs::Frame, depth);
		water.AppendSimulation(graph, resources);
		_lighting->AppendGpuLightGrid(graph, resources, *_geometry);
		if (earlyEnvironment)
		{
			_environment->Append(graph, resources);
		}
	};
	const auto afterInteraction = [&](auto& graph, auto& resources)
	{ particles.AppendSimulation(graph, resources, ParticleInputs::Interaction, depth); };
	const auto afterDepth = [&](auto& graph, auto& resources)
	{ particles.AppendSimulation(graph, resources, ParticleInputs::Depth, depth); };
	_schedule = _geometry->Plan(slot, append, prelude, singleDirect, afterInteraction, afterDepth);
	_executor->Reserve(_schedule);
	const auto record = [&](BistroRender::Id op, uint32_t list, ID3D12GraphicsCommandList* commands)
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
		completion = _executor->Execute(_schedule, _geometry->PlannedResources(slot), record, prefix);
	}
	catch (...)
	{
		_post->Reconstruction().CancelRecording();
		throw;
	}
	_post->Submitted(slot);
	_environment->Submitted(completion);
	_lighting->Submitted(completion);
	return completion;
}

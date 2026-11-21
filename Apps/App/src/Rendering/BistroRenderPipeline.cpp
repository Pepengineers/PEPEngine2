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
AutoConsoleVariable<int> AOAdapter(L"App.AO.Adapter", -1,
    L"Secondary DXGI index for Bistro AO. -1 uses local AO. Remote grids: SSAO/CACAO full or half, HBAO half, XeGTAO full.",
    EConsoleVariableFlags::Default, [](int value) { return value >= -1; });
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
	const auto ao = GetAmbientOcclusionSettings();
	const int requestedAdapter = AOAdapter.GetValue();
	_aoAdapter = BistroRender::SceneAO::SupportsRemote(ao, _geometry->DepthNormalsEnabled()) ? requestedAdapter : -1;
	if (requestedAdapter >= 0 && _aoAdapter < 0 && ao.IsActive())
	{
		if (!_reportedLocalAO)
			Log::Warning("RenderGraph", "Using local AO: the selected algorithm, grid or prepass does not support MGPU");
		_reportedLocalAO = true;
	}
	else
		_reportedLocalAO = false;
	_cloudPicture->UpdateSettings(_executor, _aoAdapter, BistroRender::SceneParticles::SecondaryAdapter());
	++_frameNumber;
	const UINT slot = c.Primary.CurrFrameConstantsIndex;
	_executor->Begin(slot);
	_post->Textures().RetireReadback(slot);
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
	_geometry->PrepareVegetation(c, c.Common.GameTimer->DeltaTime());
	c.Runtime.PrepareReflectionScene();
}

uint64_t BistroRenderPipeline::ExecuteFrame(RenderPipelineContext& c, GDX12CommandList* prefix,
                                            const std::function<void(GDX12CommandList*)>& finish)
{
	const auto slot = c.Primary.CurrFrameConstantsIndex;
	_executorSlot = slot;
	_geometry->Freeze(slot, c.Common.ActiveCameraCBufferIndex, c.Runtime.FreezeSkeletalMeshes());
	const auto scenes = BenchmarkEngine::GetLocator().GetModule<SceneManagerModule>();
	const auto* world = scenes->GetWorld();
	const double delta = c.Common.GameTimer ? c.Common.GameTimer->DeltaTime() : 0;
	_environment->Freeze(world && !world->IsPaused() ? delta * world->GetTimeScale() : 0);
	const bool remoteAO = _aoAdapter >= 0 && _aoAdapter == _cloudPicture->SubmissionAdapter();
	_lighting->Freeze(*_geometry, slot, remoteAO ? _executor->Submissions() : nullptr);
	auto& particles = _lighting->Particles();
	auto& water = _lighting->Water();
	using ParticleInputs = BistroRender::SceneParticles::Inputs;
	auto* depth = _geometry->Depth(slot);
	particles.BeginSimulation(_geometry->InteractionField(), depth, *_executor, _cloudPicture->SubmissionAdapter(),
	    world && !world->IsPaused() ? world->GetTimeScale() : 0.f);
	particles.PrepareOpaque(*_geometry, slot);
	_post->BeginPlan();
	auto& textures = _post->Textures();
	using View = BistroRender::SceneTextureInspector::View;
	const bool inspect = Engine::UI::DebugUIVisible();
	if ((inspect || textures.NeedsReadback()) && _geometry->PrepassNormals(slot))
	{
		textures.Name("Scene.DepthNormals.Depth", depth, View::Depth);
		textures.Name("Scene.DepthNormals.Normals", _geometry->PrepassNormals(slot), View::Normals);
	}
	water.Prepare(float(delta));
	water.SetSceneInputs(depth, _geometry->GBuffer(slot, 1));
	auto* target = c.BackBuffer->GetCurrentBuffer();
	const bool singleDirect = !AsyncQueues.GetValue();
	const bool earlyEnvironment = !singleDirect && _environment->AsyncComputeEnabled();
	const auto append = [&](BistroRender::Graph& graph, std::vector<ID3D12Resource*>& resources)
	{
		if (inspect || textures.NeedsReadback())
		{
			textures.Name("Scene.Depth", depth, View::Depth);
			textures.Name("Scene.AO", _lighting->AOTexture(slot), View::Scalar);
			textures.Name("Scene.AO.Reprojected", _lighting->ReprojectedAOTexture(slot), View::Scalar);
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
		if (_cloudPicture->IsRemote()) _cloudPicture->Append(graph, resources);
		const auto posters = _post->AppendPosters(graph, resources);
		_geometry->AppendWorldCanvases(graph, slot, posters);
		_lighting->SetHDRContinuation(
		    [&, inspect](auto& g, auto& bindings, auto* hdr)
		    {
			    if (inspect || textures.NeedsReadback())
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
		_post->AppendTextureReadback(graph, resources, *target);
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
		if (!_cloudPicture->IsRemote()) _cloudPicture->Append(graph, resources);
		particles.AppendSimulation(graph, resources, ParticleInputs::Frame, depth);
		water.AppendSimulation(graph, resources);
		_lighting->AppendGpuLightGrid(graph, resources, *_geometry);
		if (earlyEnvironment)
		{
			_environment->Append(graph, resources);
		}
	};
	const auto afterInteraction = [&](auto& graph, auto& resources)
	{
		particles.AppendSimulation(graph, resources, ParticleInputs::Interaction, depth);
		particles.AppendEarlyOpaquePreparation(graph, resources);
	};
	const auto afterDepth = [&](auto& graph, auto& resources)
	{
		particles.AppendSimulation(graph, resources, ParticleInputs::Depth, depth);
		particles.AppendOpaquePrepass(graph, resources, *_geometry, slot);
	};
	const auto afterDepthNormals = [&](auto& graph, auto& resources, BistroRender::Id ready)
	{
		_lighting->AppendEarlyAO(graph, resources, ready);
		if (!textures.NeedsReadback("Scene.DepthNormals.Normals") &&
		    !textures.NeedsReadback("Scene.DepthNormals.Depth")) return;
		const auto first = graph.Operations.size();
		_post->AppendTextureReadback(graph, resources, *target);
		if (graph.Operations.size() != first)
			graph.Operations.back().Dependencies.push_back(ready);
	};
	_schedule = _geometry->Plan(slot, append, prelude, singleDirect, afterInteraction, afterDepth, afterDepthNormals);
	_executor->Reserve(_schedule);
	const auto record = [&](BistroRender::Id op, uint32_t list, ID3D12GraphicsCommandList* commands)
	{
		if (!_cloudPicture->Record(op, commands) && !_environment->Record(op, commands) &&
		    !_lighting->Record(op, commands) && !_post->Record(op, commands))
		{
			_geometry->Record(slot, op, list, commands);
		}
	};
	auto waits = _cloudPicture->Waits(_schedule);
	std::vector<BistroRender::Executor::PacketExchange> exchanges;
	particles.AddExchanges(_schedule, waits, exchanges);
	if (auto ao = _lighting->AOExchange(_schedule)) exchanges.push_back(std::move(*ao));
	uint64_t completion;
	try
	{
		completion = _executor->Execute(_schedule, _geometry->PlannedResources(slot), record, prefix, nullptr, nullptr, waits,
		                                exchanges);
	}
	catch (...)
	{
		_cloudPicture->Submitted(_executor->Completions());
		particles.Submitted(_executor->Completions());
		_lighting->AOSubmitted(_executor->Completions());
		_post->Reconstruction().CancelRecording();
		throw;
	}
	_cloudPicture->Submitted(_executor->Completions());
	particles.Submitted(_executor->Completions());
	_lighting->AOSubmitted(_executor->Completions());
	_post->Submitted(slot);
	_environment->Submitted(completion);
	_lighting->Submitted(completion);
	return completion;
}


void BistroRenderPipeline::FramePresented()
{
	_cloudPicture->Presented();
	_lighting->Particles().Presented(_frameNumber);
	_lighting->AOPresented(_frameNumber, _executor->Completions());
	if (_reportedDepthNormals || !_geometry->PrepassNormals(_executorSlot)) return;
	_reportedDepthNormals = true;
	for (size_t i = 0; i < _schedule.Packets.size(); ++i)
	{
		const auto& name = _schedule.Packets[i].Name;
		if (name.find("DepthNormals") != std::string::npos || name == BistroRender::GBufferOperationName ||
		    name == "particle simulation from depth prepass" || name == "opaque particles frustum cull" ||
		    name == "particle simulation from interaction field" || name == "opaque particles G-buffer (read-only depth)" ||
		    name == "GPU0 export particle interaction field" || name == "GPU0 import particle snapshot" ||
		    name == "GPU0 export particle depth and camera" || name == "transparent particles cull and global sort" ||
		    name == "read back scene texture" || name.find(" GPU0 export inputs") != std::string::npos ||
		    name.find(" GPU0 import ") != std::string::npos || name == "XeGTAO denoise")
			Log::Info("RenderGraph", "DepthNormals frame=", _frameNumber, " packet=", i, " ", name);
	}
}

void BistroRenderPipeline::PauseParticleClock()
{
	if (_lighting) _lighting->Particles().PauseIndependentClock();
}

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
AutoConsoleVariable<int> AOAdapter(L"Render.AO.Adapter", -1,
    L"Hardware GPU index for Bistro AO. -1 uses local AO. Remote grids: SSAO/CACAO full or half, HBAO half, XeGTAO full.");
AutoConsoleVariable<int> PostAdapter(L"Render.Post.Adapter", -1,
    L"DXGI index for Bistro postprocessing and presentation. -1 uses the scene device.",
    EConsoleVariableFlags::Default, [](int index) { return index >= -1; });
AutoConsoleVariable<bool> AsyncQueues(L"Render.Queues.AsyncEnabled", true,
                                      L"Place explicit local work on COMPUTE/COPY; false uses DIRECT.",
                                      EConsoleVariableFlags::Default);

}

void BistroRenderPipeline::Initialize(RenderPipelineContext& c, const std::vector<DeviceDesc>& adapters)
{
	c.Primary.SRV_UAV_Heap->UseTransientDescriptorTail();
	_post = std::make_unique<BistroRender::ScenePostUI>(c);
	ConfigureBistroProps(*BenchmarkEngine::GetLocator().GetModule<SceneManagerModule>(), App::GetInstance()->GetJobs(), _waves);
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
	ConsoleModule console;
	const int primaryIndex = console.FindConsoleVariable(L"Render.PrimaryAdapter").GetInt();
	auto submissions = std::make_shared<BistroRender::QueueSubmission>(*c.Primary.Device,
	    adapters, primaryIndex);
	Log::Info("Renderer", "Bistro primary GPU ", submissions->AdapterLabel(0));
	_executor = std::make_unique<BistroRender::Executor>(std::move(submissions), 0, UINT(c.Primary.FrameConstants.size()));
	_lighting->SetWaterRetirement([this] { _executor->Drain(); });
	_environment->SetRetirement([this] { _executor->Drain(); });
	Log::Info("Renderer", "Bistro packet renderer initialized; one rendering path");
}

void BistroRenderPipeline::UpdateFrameSettings(RenderPipelineContext& c)
{
	_lighting->UpdateLightGridOwner(*_executor);
	const auto& submit = _executor->Submissions();
	const auto owner = submit->SelectAdapter(PostAdapter.GetValue());
	if (!_post->UpdatePostOwner(c, submit, owner))
	{
		if (_post->NeedsResize()) DrainExecution();
		return;
	}
	_bindings.PrimaryPasses = _post->Passes();
	_bindings.PrimaryPasses.push_back(&_sceneFlags);
	_bindings.Reconstruction = &_post->Reconstruction();
	c.Resize();
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
	_cloudPicture->UpdateSettings(*_executor);
	++_frameNumber;
	const UINT slot = c.Primary.CurrFrameConstantsIndex;
	_executor->Begin(slot);
	_geometry->RetireSkinning(slot, *_executor->Submissions());
	_lighting->RetireShadowSkinInputs(slot);
	_post->Textures().RetireReadback(0, slot);
	_animationTime += (std::max)(0.0, double(c.Common.GameTimer->DeltaTime()));
	const auto animationTime = _benchmarkTime.value_or(_animationTime);
	_cloudPicture->Prepare(slot, animationTime);
	_post->PrepareMaterials(*_executor, _frameNumber, animationTime, _cloudPicture->Image());
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
	_geometry->PrepareVegetation(c, c.Common.GameTimer->DeltaTime(), *_executor);
	c.Runtime.PrepareReflectionScene();
}

uint64_t BistroRenderPipeline::ExecuteFrame(RenderPipelineContext& c, GDX12CommandList* prefix,
                                            const std::function<void(GDX12CommandList*)>& finish)
{
	const auto slot = c.Primary.CurrFrameConstantsIndex;
	_executorSlot = slot;
	const auto& submissions = _executor->Submissions();
	auto skin = c.Runtime.FreezeSkeletalMeshes([&](int requested)
	{
		const auto owner = submissions->SelectAdapter(_benchmarkLocalSkinning ? -1 : requested);
		return std::make_pair(owner, &submissions->Device(owner));
	});
	_geometry->Freeze(slot, c.Common.ActiveCameraCBufferIndex, std::move(skin), submissions);
	const auto transparencyOwner = _transparencyOwner = _lighting->Transparency().Begin(submissions, slot, _frameNumber);
	const auto scenes = BenchmarkEngine::GetLocator().GetModule<SceneManagerModule>();
	const auto* world = scenes->GetWorld();
	const double delta = c.Common.GameTimer ? c.Common.GameTimer->DeltaTime() : 0;
	_environment->Freeze(world && !world->IsPaused() ? delta * world->GetTimeScale() : 0, *_executor, _frameNumber);
	_lighting->Freeze(*_geometry, slot, submissions, submissions->SelectAdapter(_aoAdapter));
	auto& particles = _lighting->Particles();
	auto& water = _lighting->Water();
	auto& grass = _geometry->Grass();
	using ParticleInputs = BistroRender::SceneParticles::Inputs;
	auto* depth = _geometry->Depth(slot);
	particles.BeginSimulation(_geometry->InteractionField(), depth, *_executor, submissions->SelectAdapter(BistroRender::SceneParticles::SecondaryAdapter()),
	    world && !world->IsPaused() ? world->GetTimeScale() : 0.f, transparencyOwner != 0);
	particles.PrepareOpaque(*_geometry, slot);
	_post->BeginPlan(*_executor, _frameNumber);
	auto& textures = _post->Textures();
	using View = BistroRender::SceneTextureInspector::View;
	const bool inspect = BistroRender::SceneTextureInspector::Visible();
	if ((inspect || textures.NeedsReadback()) && _geometry->PrepassNormals(slot))
	{
		textures.Name("Scene.Depth", depth, View::Depth);
		if (textures.NeedsReadback("Scene.DepthNormals.Depth"))
			textures.Name("Scene.DepthNormals.Depth", depth, View::Depth);
		textures.Name("Scene.DepthNormals.Normals", _geometry->PrepassNormals(slot), View::Normals);
	}
	water.Prepare(float(delta), *_executor, submissions->SelectAdapter(BistroRender::SceneWater::SecondaryAdapter()));
	const std::array placements{
		_cloudPicture->ExecutionDevice(), _lighting->AOExecutionDevice(),
		particles.ExecutionDevice(), water.ExecutionDevice(), grass.WindExecutionDevice()};
	constexpr std::array names{"CloudPicture", "AO", "Particles", "Water", "ProjectedGrid wind"};
	for (size_t i = 0; i < placements.size(); ++i)
	{
		if (placements[i] == _reportedDevices[i]) continue;
		_reportedDevices[i] = placements[i];
		Log::Info("RenderGraph", names[i], " execution GPU ", submissions->AdapterLabel(placements[i]));
	}
	water.SetSceneInputs(depth, _geometry->GBuffer(slot, 1));
	auto* target = c.BackBuffer->GetCurrentBuffer();
	const bool singleDirect = !AsyncQueues.GetValue();
	const bool earlyEnvironment = !singleDirect && _environment->AsyncComputeEnabled();
	const auto append = [&](BistroRender::Graph& graph, std::vector<ID3D12Resource*>& resources)
	{
		if (inspect || textures.NeedsReadback())
		{
			if (!_geometry->PrepassNormals(slot)) textures.Name("Scene.Depth", depth, View::Depth);
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
		// In the combined path opaque particles finish writing depth with their G-buffer draw.
		if (!_geometry->PrepassNormals(slot)) _lighting->Transparency().PrepareOpaqueDepth(graph, resources, slot, depth);
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
				for (const auto& poster : posters) op.Accesses.push_back(poster.Texture);
			}
		}
	};
	const auto prelude = [&](BistroRender::Graph& graph, std::vector<ID3D12Resource*>& resources)
	{
		if (!_cloudPicture->IsRemote()) _cloudPicture->Append(graph, resources);
		particles.AppendSimulation(graph, resources, ParticleInputs::Frame, depth);
		water.AppendSimulation(graph, resources);
		_lighting->AppendGpuLightGrid(graph, resources);
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
		if (_geometry->PrepassNormals(slot)) _lighting->Transparency().PrepareOpaqueDepth(graph, resources, slot, depth);
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
	const auto& schedule = *_schedule;
	_executor->Reserve(schedule);
	const auto record = [&](BistroRender::Id op, uint32_t list, ID3D12GraphicsCommandList* commands)
	{
		if (!_cloudPicture->Record(op, commands) && !_environment->Record(op, commands) &&
		    !_lighting->Record(op, commands) && !_post->Record(op, commands))
		{
			_geometry->Record(slot, op, list, commands);
		}
	};
	auto waits = _cloudPicture->Waits(schedule);
	_environment->AddAtmosphereWaits(schedule, waits);
	water.AddWaits(schedule, waits);
	grass.AddWindWaits(schedule, waits);
	std::vector<BistroRender::Executor::PacketExchange> exchanges;
	_post->AddPosterWaits(schedule, waits);
	particles.AddExchanges(schedule, waits, exchanges);
	if (auto ao = _lighting->AOExchange(schedule)) exchanges.push_back(std::move(*ao));
	_lighting->AddProbeExchanges(schedule, waits, exchanges);
	_lighting->Transparency().AddExchange(schedule, exchanges);
	_post->AddPostExchange(schedule, exchanges);
	uint64_t completion;
	try
	{
		_lighting->SubmitLightGrid(schedule, waits);
		_post->SubmitHud(schedule, waits);
		_geometry->SubmitSkinning(slot, schedule, waits);
		completion = _executor->Execute(schedule, _geometry->PlannedResources(slot), record, prefix, nullptr, nullptr, waits,
		                                exchanges);
		if (_benchmarkTime)
		{
			_benchmarkSky = c.Primary.Sky.Source;
			_benchmarkLinks.clear();
			for (const auto& wait : waits)
				if (wait.Token && wait.Token.Device != 0)
					_benchmarkLinks.push_back({BistroRender::Invalid, wait.Token.Device, wait.Packet});
			for (size_t i = 0; i < exchanges.size(); ++i)
			{
				const auto token = _executor->ExchangeCompletions()[i];
				if (token && token.Device != 0)
					_benchmarkLinks.push_back({exchanges[i].Export, token.Device, exchanges[i].Import});
			}
		}
		if (std::exchange(_checkPanelPlacement, false))
		{
			_panelOwners = PanelAdapters();
			++_panelRevision; // Queue targets can change without changing GPU owners.
		}
		_post->Textures().ReadbackSubmitted(0, slot);
	}
	catch (...)
	{
		try { _lighting->DrainLightGrid(); }
		catch (...) { OutputDebugStringA("Bistro light grid retirement failed; preserving the original exception.\n"); }
		try { _post->DrainHud(); _post->DrainPost(); }
		catch (...) { OutputDebugStringA("Bistro HUD retirement failed; preserving the original exception.\n"); }
		_post->Reconstruction().CancelRecording();
		const auto& completions = _executor->Completions();
		try
		{
			if (completions.empty())
			{
				// A skinning owner may have submitted work before GPU0 had any packets to report.
				_geometry->DrainSkinning();
			}
			else
			{
				_cloudPicture->Submitted(completions);
				_environment->AtmosphereSubmitted(_frameNumber, completions);
				_post->PostersSubmitted(_frameNumber, completions);
				water.Submitted(completions);
				grass.WindSubmitted(_frameNumber, completions);
				particles.Submitted(completions);
				_lighting->AOSubmitted(completions);
				_lighting->ProbesSubmitted(completions);
				_lighting->Transparency().Submitted(completions);
			}
		}
		catch (...)
		{
			OutputDebugStringA("Bistro submission cleanup failed; preserving the original exception.\n");
		}
		throw;
	}
	_geometry->SkinningSubmitted(slot, _frameNumber, _executor->Completions().back());
	_cloudPicture->Submitted(_executor->Completions());
	_environment->AtmosphereSubmitted(_frameNumber, _executor->Completions());
	water.Submitted(_executor->Completions());
	grass.WindSubmitted(_frameNumber, _executor->Completions());
	particles.Submitted(_executor->Completions());
	_lighting->AOSubmitted(_executor->Completions());
	_lighting->ProbesSubmitted(_executor->Completions());
	_lighting->Transparency().Submitted(_executor->Completions());
	_post->HudSubmitted(_executor->Completions());
	_post->PostersSubmitted(_frameNumber, _executor->Completions());
	_post->Submitted(slot);
	_environment->Submitted(completion);
	_lighting->Submitted(completion);
	return completion;
}


void BistroRenderPipeline::FramePresented()
{
	_afterPresent = std::chrono::steady_clock::now();
	++_presentedFrames;
	_cloudPicture->Presented();
	_post->HudPresented();
	_post->PostPresented();
	_lighting->Transparency().Presented();
	_lighting->Water().Presented(_frameNumber, _executor->Completions());
	_lighting->Particles().Presented(_frameNumber);
	_lighting->AOPresented(_frameNumber, _executor->Completions());
	_lighting->LightGridPresented(_frameNumber, *_schedule, _executor->Completions());
	if (_reportedDepthNormals || !_geometry->PrepassNormals(_executorSlot)) return;
	_reportedDepthNormals = true;
	for (size_t i = 0; i < _schedule->Packets.size(); ++i)
	{
		const auto& name = _schedule->Packets[i].Name;
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

std::vector<std::pair<std::string, std::string>> BistroRenderPipeline::BenchmarkAdapters() const
{
	const auto& devices = *_executor->Submissions();
	const auto [skyView, aerial] = _environment->AtmosphereDevices();
	std::vector<std::pair<std::string, std::string>> adapters{
	    {"SceneAdapter", devices.AdapterLabel(0)},
	    {"AOAdapter", devices.AdapterLabel(_lighting->AOExecutionDevice())},
	    {"SkyViewAdapter", devices.AdapterLabel(skyView)},
	    {"AerialPerspectiveAdapter", devices.AdapterLabel(aerial)},
	    {"LightGridAdapter", devices.AdapterLabel(_lighting->LightGridDevice())},
	    {"SunShadowAdapter", devices.AdapterLabel(_lighting->SunShadowDevice())},
	    {"ParticleAdapter", devices.AdapterLabel(_lighting->Particles().ExecutionDevice())},
	    {"WaterAdapter", devices.AdapterLabel(_lighting->Water().ExecutionDevice())},
	    {"CloudPictureAdapter", devices.AdapterLabel(_cloudPicture->ExecutionDevice())},
	    {"GrassWindAdapter", devices.AdapterLabel(_geometry->Grass().WindExecutionDevice())},
	    {"TransparencyAdapter", devices.AdapterLabel(_transparencyOwner)},
	};
	for (const auto& [name, id] : _post->ExecutionDevices())
		adapters.emplace_back(name, id == BistroRender::Invalid ? "inactive" : devices.AdapterLabel(id));
	return adapters;
}

std::vector<std::pair<std::string, std::string>> BistroRenderPipeline::PanelAdapters() const
{
	auto adapters = BenchmarkAdapters();
	adapters.erase(std::remove_if(adapters.begin(), adapters.end(), [](const auto& entry) {
		return entry.first == "SunShadowAdapter";
	}), adapters.end());
	std::vector<std::pair<std::string, BistroRender::Id>> owners;
	_lighting->AppendPanelOwners(owners);
	_geometry->AppendSkinningOwners(_executorSlot, owners);
	for (const auto& [name, id] : owners)
		adapters.emplace_back(name, _executor->Submissions()->AdapterLabel(id));
	return adapters;
}

std::function<void()> BistroRenderPipeline::CaptureBenchmarkPhysics(World& world)
{
	auto physics = BenchmarkEngine::GetLocator().GetModule<PhysicsModule>();
	return [restore = physics->CaptureReplayState(world), waves = _waves, timeline = _waves->GetTimeline()]
	{
		restore();
		waves->RestoreTimeline(timeline);
	};
}

void BistroRenderPipeline::PauseIndependentClocks()
{
	if (!_lighting) return;
	_lighting->Particles().PauseIndependentClock();
	_lighting->Water().PauseIndependentClock();
}

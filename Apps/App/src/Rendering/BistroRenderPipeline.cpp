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
AutoConsoleVariable<bool> AsyncQueues(L"App.Async.Queues", true,
                                      L"Place explicit local work on COMPUTE/COPY; false uses DIRECT.",
                                      EConsoleVariableFlags::Default);

std::unique_ptr<GDX12Device> OpenSecondary(GDX12Device& primary, int index)
{
	if (index < 0)
	{
		return {};
	}
	ComPtr<IDXGIFactory6> factory;
	ThrowIfFailed(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
	ComPtr<IDXGIAdapter1> adapter;
	ThrowIfFailed(factory->EnumAdapters1(UINT(index), &adapter));
	DXGI_ADAPTER_DESC1 desc{};
	ThrowIfFailed(adapter->GetDesc1(&desc));
	if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
	{
		throw std::invalid_argument("Bistro requires a hardware adapter");
	}
	const auto primaryLuid = primary.GetAdapterLuid();
	if (desc.AdapterLuid.HighPart == primaryLuid.HighPart && desc.AdapterLuid.LowPart == primaryLuid.LowPart)
	{
		return {};
	}
	ComPtr<IDXGIAdapter4> hardware;
	ThrowIfFailed(adapter.As(&hardware));
	auto device = std::make_unique<GDX12Device>();
	ThrowIfFailed(device->Initialize(hardware));
	return device;
}
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
	_submissions = std::make_shared<BistroRender::QueueSubmission>(*c.Primary.Device);
	_executor = std::make_unique<BistroRender::Executor>(_submissions, 0,
	                                                     UINT(c.Primary.FrameConstants.size()));
	_lighting->SetWaterRetirement([this] { _executor->Drain(); });
	_environment->SetRetirement([this] { _executor->Drain(); });
	Log::Info("Renderer", "Bistro packet renderer initialized; one rendering path");
}

void BistroRenderPipeline::PrepareMaterialBindings(RenderPipelineContext& c)
{
	auto candidate = _submissions;
	int selection = _secondarySelection;
	const auto select = [&](int requested)
	{
		if (requested != _secondarySelection)
		{
			auto secondary = OpenSecondary(*c.Primary.Device, requested);
			if (secondary || _submissions->HasSecondary())
			{
				candidate = std::make_shared<BistroRender::QueueSubmission>(*c.Primary.Device, std::move(secondary));
			}
			selection = requested;
		}
		return candidate;
	};
	if (_cloudPicture->UpdateSettings(select, [this] { _executor->Drain(); }))
	{
		_secondarySelection = selection;
		if (candidate != _submissions)
		{
			_executor = std::make_unique<BistroRender::Executor>(candidate, 0, UINT(c.Primary.FrameConstants.size()));
			_submissions = std::move(candidate);
		}
	}
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
		particles.AppendOpaque(graph, resources, *_geometry, slot);
		if (!earlyEnvironment)
		{
			_environment->Append(graph, resources);
		}
		const auto posters = _post->AppendPosters(graph, resources);
		_geometry->AppendWorldCanvases(graph, slot, posters);
		_lighting->SetHDRContinuation(
		    [&](auto& g, auto& bindings, auto* hdr)
		    {
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

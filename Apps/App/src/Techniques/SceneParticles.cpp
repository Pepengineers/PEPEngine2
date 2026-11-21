#include "SceneParticles.h"
#include "SceneGeometry.h"
#include "Common/ConsoleVariables.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12TextureResource.h"
#include "Engine.RendererDX12/GDX12MotionBlurSettings.h"
#include "Engine.RendererDX12/GDX12FrameGeneration.h"
#include "Engine.Core/Logging/Logger.h"
#include <cassert>

namespace BistroRender
{
namespace
{
AutoConsoleVariable<bool> AsyncSimulation(L"App.Async.Particles", false,
                                          L"Run particle pool initialization and simulation on the local COMPUTE queue.",
                                          EConsoleVariableFlags::Default);
AutoConsoleVariable<int> Adapter(L"App.Particles.Adapter", -1,
    L"Secondary DXGI index for marked independent particle emitters. -1 uses GPU0.",
    EConsoleVariableFlags::Default, [](int value) { return value >= -1; });
AutoConsoleVariable<int> CrossSync(L"App.Particles.CrossSync", 1,
    L"Remote particles: 1 current step, 0 exact previous step, 3 newest complete frame-driven step.",
    EConsoleVariableFlags::Default, [](int value) { return value == 0 || value == 1 || value == 3; });
AutoConsoleVariable<bool> LogSnapshots(L"App.Particles.LogSnapshots", false,
    L"Read back visible remote particle counts for short MGPU diagnostics.");

constexpr State CopySource = D3D12_RESOURCE_STATE_COPY_SOURCE;
constexpr State CopyDest = D3D12_RESOURCE_STATE_COPY_DEST;

auto SnapshotResources(const GDX12ParticleRenderSource& source)
{
	return std::array{source.Particles, source.Alive, source.Counters, source.Arguments};
}

State BoundaryState(ID3D12Resource* resource)
{
	const auto desc = resource->GetDesc();
	if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
	{
		return D3D12_RESOURCE_STATE_GENERIC_READ;
	}
	return desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE ? State(D3D12_RESOURCE_STATE_COMMON)
	                                                             : State(D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
}
Access ResourceAccess(Graph& graph, std::vector<ID3D12Resource*>& bindings, GDX12Resource* resource, Mode mode,
                      State state = State(-1))
{
	auto* native = resource->D3DResource.Get();
	D3D12_HEAP_PROPERTIES heap{};
	ThrowIfFailed(native->GetHeapProperties(&heap, nullptr));
	return {NativeWork::ResourceFor(graph, bindings, native, "particle resource"),
	        heap.Type == D3D12_HEAP_TYPE_UPLOAD ? Mode::Read : mode,
	        heap.Type == D3D12_HEAP_TYPE_UPLOAD || state == State(-1) ? BoundaryState(native) : state};
}
void AddResources(Graph& graph, std::vector<ID3D12Resource*>& bindings, std::vector<Access>& accesses,
                  const std::vector<GDX12Resource*>& resources, Mode mode)
{
	for (auto* resource : resources)
	{
		accesses.push_back(ResourceAccess(graph, bindings, resource, mode));
	}
}
void AddAccesses(Graph& graph, std::vector<ID3D12Resource*>& bindings, std::vector<Access>& accesses,
                 const std::vector<GDX12ResourceAccess>& resources)
{
	for (const auto& resource : resources)
	{
		accesses.push_back(ResourceAccess(graph, bindings, resource.Resource,
		                                  resource.Usage == GDX12AccessMode::Read    ? Mode::Read
		                                  : resource.Usage == GDX12AccessMode::Write ? Mode::Write
		                                                                             : Mode::ReadWrite,
		                                  resource.State));
	}
}
} // namespace

struct SceneParticles::Remote
{
	struct Publication
	{
		QueueSubmission::TransferBuffer Transfer;
		CompletionToken Ready, Reader;
		uint64_t Generation = 0, Version = 0;
		std::vector<GDX12ParticleEmitter> Snapshot;
	};
	struct ImportedEmitter
	{
		std::array<GDX12Resource, 4> Buffers;
		std::array<GDX12Descriptor, 3> Views;
		GDX12ParticleRenderSource Bindings;
	};

	std::shared_ptr<QueueSubmission> Submit;
	GDX12DeviceResources Secondary;
	GDX12ParticleSimulation Solver;
	Executor GPU1;
	std::vector<EmitterLayout> Layout;
	std::vector<ImportedEmitter> Imported;
	std::vector<Publication> Ring;
	std::vector<uint64_t> Revisions;
	UINT InputSlot = 0, NextOutput = 0;
	Publication *Selected = nullptr, *LastOrder = nullptr;
	uint64_t Generation = 0, Ordered = 0, PresentedVersion = 0;
	uint64_t CachedVersion = 0;
	std::vector<GDX12ParticleEmitter> PresentedSnapshot;
	struct Statistics
	{
		uint64_t Frames = 0, Exports = 0, Imports = 0, Repeats = 0, Skipped = 0, Empty = 0;
		uint64_t InputWaits = 0, OutputWaits = 0;
	} Stats;
	double GameTime = 0;
	int Mode = 1;

	Remote(GDX12DeviceResources& primary, const std::shared_ptr<QueueSubmission>& submit,
	       const std::vector<EmitterLayout>& layout, std::vector<GDX12ParticleEmitter> snapshot,
	       const GDX12ParticleSettings& settings)
	    : Submit(submit), GPU1(Submit, 1, UINT(primary.FrameConstants.size()), 1), Layout(layout),
	      Imported(layout.size()), Ring(primary.FrameConstants.size() + 1)
	{
		Secondary.Device = &Submit->Device(1);
		Secondary.FrameConstants.resize(primary.FrameConstants.size()); // Solver needs only the upload ring size.
		Secondary.SRV_UAV_Heap = std::make_shared<GDX12DescriptorHeap>(Secondary.Device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
		    SceneDescriptorStart + UINT(12 * layout.size() + 1), D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
		Secondary.ParticleEmitters = std::move(snapshot);
		Solver.BeginFrame(&Secondary, settings, false);
		auto* heap = primary.SRV_UAV_Heap.get();
		const auto frame = Solver.RenderFrame();
		UINT64 bytes = 0;
		for (size_t i = 0; i < Imported.size(); ++i)
		{
			auto& dest = Imported[i];
			const auto& source = frame.Sources.at(i);
			dest.Bindings = source;
			dest.Bindings.Particles = &dest.Buffers[0];
			dest.Bindings.Alive = &dest.Buffers[1];
			dest.Bindings.Counters = &dest.Buffers[2];
			dest.Bindings.Arguments = &dest.Buffers[3];
			const auto sources = SnapshotResources(source);
			for (size_t field = 0; field < sources.size(); ++field)
			{
				const auto width = sources[field]->D3DResource->GetDesc().Width;
				const auto desc = CD3DX12_RESOURCE_DESC::Buffer(width);
				auto& resource = dest.Buffers[field];
				resource.D3DResource = primary.Device->CreateResource(D3D12_HEAP_TYPE_DEFAULT, desc, D3D12_RESOURCE_STATE_COMMON);
				bytes += width;
				if (field >= dest.Views.size()) continue;
				D3D12_SHADER_RESOURCE_VIEW_DESC view{};
				view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
				view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
				view.Buffer.NumElements = field == 2 ? UINT(width / sizeof(UINT)) : source.Emitter.MaxParticles;
				view.Buffer.StructureByteStride = UINT(width / view.Buffer.NumElements);
				auto& descriptor = dest.Views[field];
				descriptor.InitAsSRV(resource.D3DResource.Get(), &view, heap, heap->GetAvailableIndex(SceneDescriptorStart));
				dest.Bindings.SRVs[field] = descriptor.GPUHandle;
			}
		}
		for (auto& slot : Ring) slot.Transfer = Submit->CreateTransferBuffer(bytes);
		const auto gpu = Submit->Device(1).GetAdapterLuid();
		Log::Info("RenderGraph", "Particles MGPU solver GPU1 LUID=", gpu.HighPart, ":", gpu.LowPart,
		    " emitters=", Layout.size(), " snapshotBytes=", bytes);
	}

	void Drain() { GPU1.Drain(); }

	bool LatestReady() const { return Mode == 3; }

	void Configure(const std::vector<GDX12ParticleEmitter>& emitters, int mode)
	{
		std::vector<uint64_t> revisions;
		for (const auto& emitter : emitters) revisions.push_back(emitter.ResetRevision);
		if (revisions != Revisions || mode != Mode)
		{
			Revisions = std::move(revisions);
			Mode = mode;
			++Generation;
			Ordered = PresentedVersion = CachedVersion = 0;
			PresentedSnapshot.clear();
			GameTime = 0;
			LastOrder = nullptr;
			Stats = {};
		}
	}

	Publication* NewestCompleted()
	{
		Publication* latest = nullptr;
		for (auto& slot : Ring)
			if (slot.Generation == Generation && slot.Ready && (!latest || slot.Version > latest->Version) &&
			    Submit->Complete(slot.Ready))
				latest = &slot;
		return latest;
	}

	Publication* ReserveOutput()
	{
		const bool latestReady = LatestReady();
		auto* latest = latestReady ? NewestCompleted() : nullptr;
		for (size_t i = 0; i < Ring.size(); ++i)
		{
			auto& output = Ring[NextOutput++ % Ring.size()];
			if (&output == Selected || &output == latest) continue;
			if (latestReady && output.Ready && !Submit->Complete(output.Ready)) continue;
			if (output.Reader && !Submit->Complete(output.Reader))
			{
				if (latestReady) continue;
				++Stats.OutputWaits;
				Submit->Wait(output.Reader);
			}
			output.Reader = {};
			return &output;
		}
		return nullptr;
	}

	void Step(const std::vector<GDX12ParticleEmitter>& emitters, const GDX12ParticleSettings& settings, int mode)
	{
		Configure(emitters, mode);
		Selected = LatestReady() ? NewestCompleted() : Mode == 0 ? LastOrder : nullptr;
		auto* output = ReserveOutput();
		if (!LatestReady() && !output) throw std::logic_error("No slot for exact particle snapshot");
		Simulate(GPU1, emitters, settings, output);
		if (!LatestReady() && !Selected) Selected = LastOrder;
		if (!settings.DrawEnabled) Selected = nullptr;
	}

	CompletionToken Simulate(Executor& executor, const std::vector<GDX12ParticleEmitter>& emitters,
	                         const GDX12ParticleSettings& settings, Publication* output)
	{
		const UINT slot = InputSlot++ % UINT(Secondary.FrameConstants.size());
		if (!executor.TryBegin(slot))
		{
			++Stats.InputWaits;
			executor.Begin(slot);
		}
		Secondary.CurrFrameConstantsIndex = slot;
		Secondary.ParticleEmitters = emitters;
		Solver.BeginFrame(&Secondary, settings, false);
		const auto frame = Solver.RenderFrame();
		if (output) output->Snapshot.clear();
		double elapsed = 0;
		for (const auto& source : frame.Sources)
		{
			if (output) output->Snapshot.push_back(source.Emitter);
			const auto& e = source.Emitter;
			if (!e.Paused) elapsed = (std::max)(elapsed, double((std::min)(e.DeltaTime * e.TimeScale, e.MaxDeltaTime)));
		}
		Graph graph;
		graph.Device = 1;
		graph.FrameBoundary = false;
		graph.TerminalQueue = Queue::Compute;
		std::vector<ID3D12Resource*> bindings;
		NativeWork work;
		std::vector<Access> pools;
		for (auto* buffer : Solver.GetPoolResources())
			pools.push_back(ResourceAccess(graph, bindings, buffer, Mode::ReadWrite, UnorderedAccess));
		work.AddOwned(graph, "GPU1 initialize particle pools", pools, bindings, [this](auto* cmd) {
			Solver.InitializePools(cmd, &Secondary);
		}, "Particle initialization must not submit", {}, Queue::Compute);
		AddResources(graph, bindings, pools, Solver.GetConstantResources(slot), Mode::Read);
		work.AddOwned(graph, "GPU1 simulate independent particles", std::move(pools), bindings, [this](auto* cmd) {
			Solver.ExecuteFrameOnly(cmd, &Secondary);
		}, "Particle simulation must not submit", {}, Queue::Compute);
		if (output)
		{
			auto* shared = output->Transfer.Resources[1].Get();
			std::vector<Access> copies{{NativeWork::ResourceFor(graph, bindings, shared, "particle snapshot shared"), Mode::Write, CopyDest}};
			for (const auto& source : frame.Sources)
				for (auto* input : SnapshotResources(source))
					copies.push_back(ResourceAccess(graph, bindings, input, Mode::Read, CopySource));
			work.Add(graph, "GPU1 export particle snapshot", std::move(copies), [shared, &frame](auto* cmd) {
				UINT64 offset = 0;
				for (const auto& source : frame.Sources)
					for (auto* input : SnapshotResources(source))
					{
						auto* buffer = input->D3DResource.Get();
						const auto bytes = buffer->GetDesc().Width;
						cmd->CopyBufferRegion(shared, offset, buffer, 0, bytes);
						offset += bytes;
					}
			}, {}, Queue::Compute);
		}
		const auto schedule = Compile(graph);
		executor.Reserve(schedule);
		executor.Execute(schedule, bindings, [&](Id op, uint32_t, auto* cmd) { work.Record(op, cmd); });
		const auto completed = executor.Completions().back();
		++Ordered;
		GameTime += elapsed;
		if (output)
		{
			output->Ready = completed;
			output->Version = Ordered;
			output->Generation = Generation;
			LastOrder = output;
			++Stats.Exports;
		}
		return completed;
	}

	void AppendSources(GDX12ParticleRenderFrame& frame) const
	{
		if (!Selected) return;
		for (size_t i = 0; i < Imported.size(); ++i)
		{
			auto source = Imported[i].Bindings;
			source.Emitter = Selected->Snapshot[i];
			source.ResetMotion = !PresentedVersion || Selected->Version != PresentedVersion + 1 || source.Emitter.Paused;
			if (!source.ResetMotion)
			{
				const auto& previous = PresentedSnapshot[i];
				source.ResetMotion = previous.SizeStart != source.Emitter.SizeStart || previous.SizeEnd != source.Emitter.SizeEnd ||
				                     previous.RenderScale != source.Emitter.RenderScale;
			}
			frame.Sources.push_back(source);
		}
	}

	Id AppendImport(NativeWork& work, Graph& graph, std::vector<ID3D12Resource*>& bindings)
	{
		if (!Selected || (LatestReady() && Selected->Version == CachedVersion)) return Invalid;
		auto* shared = Selected->Transfer.Resources[0].Get();
		std::vector<Access> copies{{NativeWork::ResourceFor(graph, bindings, shared, "particle snapshot shared"), Mode::Read, CopySource}};
		for (auto& emitter : Imported)
			for (auto& buffer : emitter.Buffers)
				copies.push_back(ResourceAccess(graph, bindings, &buffer, Mode::Write, CopyDest));
		return work.Add(graph, "GPU0 import particle snapshot", std::move(copies), [this, shared](auto* cmd) {
			UINT64 offset = 0;
			for (auto& emitter : Imported)
				for (auto& dest : emitter.Buffers)
				{
					auto* buffer = dest.D3DResource.Get();
					const auto bytes = buffer->GetDesc().Width;
					cmd->CopyBufferRegion(buffer, 0, shared, offset, bytes);
					offset += bytes;
				}
		}, {}, Queue::Copy);
	}
};

SceneParticles::SceneParticles(GDX12DeviceResources& resources, RenderPipelineCommonData& common)
    : _resources(resources), _common(common), _visibilitySamples(resources.FrameConstants.size())
{
	if (!_resources.ParticleSimulation)
	{
		_resources.ParticleSimulation = std::make_unique<GDX12ParticleSimulation>();
	}
	// Merged fluid depth is an SRV/UAV, so the raster PSO always uses shader depth rejection.
	_pass.SetReadOnlyDepthEnabled(false);
	_pass.SetMotionOutputEnabled(false);
	_pass.Setup(&_resources, &_common);
}
SceneParticles::~SceneParticles()
{
	if (_remote)
	{
		try { _remote->Drain(); }
		catch (...) { (void)_remote.release(); } // A quarantined queue can still reference the buffers.
	}
	_pass.ClearDenendencies();
	if (_resources.ParticleSimulation)
	{
		_resources.ParticleSimulation->SetInteractionField({});
	}
}
void SceneParticles::Packet(Graph& graph, const std::vector<ID3D12Resource*>& bindings, const char* name,
                            std::vector<Access> accesses, std::function<void(GDX12CommandList*)> record,
                            Queue target)
{
	std::vector<Access> unique;
	for (auto access : accesses)
	{
		const auto found =
		    std::find_if(unique.begin(), unique.end(), [&](auto other) { return other.Resource == access.Resource; });
		if (found == unique.end())
		{
			unique.push_back(access);
		}
		else
		{
			if (found->Required != access.Required)
			{
				throw std::logic_error("Conflicting particle resource states");
			}
			if (found->Usage != access.Usage)
			{
				found->Usage = Mode::ReadWrite;
			}
		}
	}
	_work.AddOwned(graph, name, std::move(unique), bindings, std::move(record),
	               "Particle packets cannot submit private signals", {}, target);
}
int SceneParticles::SecondaryAdapter() { return Adapter.GetValue(); }

void SceneParticles::BeginSimulation(GDX12ParticleInteractionField interaction, Executor& primary, int activeAdapter)
{
	_work.Commands.clear();
	_import = _importPacket = Invalid;
	_sampleVisibility = false;
	auto& sample = _visibilitySamples.at(_resources.CurrFrameConstantsIndex);
	if (sample.Frame)
	{
		const D3D12_RANGE bytes{0, 2 * sizeof(UINT)};
		void* mapped = nullptr;
		ThrowIfFailed(sample.Buffer.D3DResource->Map(0, &bytes, &mapped));
		const auto* counts = static_cast<const UINT*>(mapped);
		const UINT visible = counts[1] - counts[0];
		if (sample.Frame <= 12 || (visible && !_visibilityConfirmed))
			Log::Info("RenderGraph", "Particles MGPU visible GPU0 frame=", sample.Frame,
			    " shown=", sample.Version, " remoteDrawElements=", visible);
		_visibilityConfirmed |= visible != 0;
		const D3D12_RANGE noWrites{0, 0};
		sample.Buffer.D3DResource->Unmap(0, &noWrites);
		sample.Frame = 0;
	}
	_interaction = interaction;
	auto* simulation = _resources.ParticleSimulation.get();
	simulation->SetInteractionField(interaction);
	const auto settings = GetParticleSettings();
	const auto& submit = primary.Submissions();
	const bool requested = activeAdapter >= 0 && activeAdapter == SecondaryAdapter();
	const uint64_t domain = requested ? submit->Incarnation() : 0;
	std::vector<GDX12ParticleEmitter> remoteEmitters;
	std::vector<EmitterLayout> layout;
	if (requested)
		for (const auto& emitter : _resources.ParticleEmitters)
			if (emitter.SimulateOnSecondary && !emitter.DepthCollision && !emitter.InteractionEnabled &&
			    emitter.Blend != ParticleBlend::Opaque)
			{
				remoteEmitters.push_back(emitter);
				layout.push_back({emitter.Id, ResolveParticleEmitterSettings(emitter, settings.Quality).MaxParticles});
			}
	const bool replace = domain != _attemptedDomain || _attemptedLayout != layout;
	if (replace)
	{
		if (_remote)
		{
			primary.Drain();
			_remote->Drain();
			for (auto& emitter : _resources.ParticleEmitters)
				emitter.BurstBaseline = _remote->Solver.ConsumedBurst(emitter.Id, emitter.ResetRevision);
			_remote.reset();
		}
		_attemptedDomain = domain;
		_attemptedLayout = layout;
		_attemptedRemote = false;
	}
	if (!_remote && requested && !layout.empty() && settings.Enabled && (replace || !_attemptedRemote))
	{
		_attemptedRemote = true;
		for (auto& emitter : remoteEmitters)
		{
			const auto& live = *std::find_if(_resources.ParticleEmitters.begin(), _resources.ParticleEmitters.end(),
			    [&](const auto& source) { return source.Id == emitter.Id; });
			emitter.BurstBaseline = (std::max)(live.BurstBaseline, simulation->ConsumedBurst(emitter.Id, emitter.ResetRevision));
		}
		try { _remote = std::make_unique<Remote>(_resources, submit, layout, remoteEmitters, settings); }
		catch (const DxException& e) { Log::Warning("RenderGraph", "Local particles fallback: ", e.ToString()); }
		catch (const std::exception& e) { Log::Warning("RenderGraph", "Local particles fallback: ", e.what()); }
	}
	if (layout.empty()) _attemptedRemote = false;
	std::vector<uint64_t> remoteIds;
	if (_remote)
	{
		for (const auto& item : layout) remoteIds.push_back(item.Id);
		_remote->Selected = nullptr;
		if (settings.Enabled) _remote->Step(remoteEmitters, settings, CrossSync.GetValue());
	}
	simulation->BeginFrame(&_resources, _common, &remoteIds);
	_renderFrame = simulation->RenderFrame();
	_firstRemote = UINT(_renderFrame.Sources.size());
	if (_remote) _remote->AppendSources(_renderFrame);
	for (auto& group : _groups)
	{
		group.clear();
	}
	for (const auto& emitter : _resources.ParticleEmitters)
	{
		if (std::find(remoteIds.begin(), remoteIds.end(), emitter.Id) != remoteIds.end()) continue;
		const auto inputs = emitter.DepthCollision ? Inputs::Depth
		                  : emitter.InteractionEnabled && interaction.Resource ? Inputs::Interaction : Inputs::Frame;
		_groups[size_t(inputs)].push_back(emitter.Id);
	}
}

void SceneParticles::AddWaits(const Schedule& schedule, std::vector<PacketWait>& waits)
{
	if (_import == Invalid) return;
	const auto packet = std::find_if(schedule.Packets.begin(), schedule.Packets.end(),
	    [this](const auto& value) { return value.Operation == _import; });
	if (packet == schedule.Packets.end()) throw std::logic_error("Particle import packet is missing");
	_importPacket = Id(packet - schedule.Packets.begin());
	if (!_remote->LatestReady())
		waits.push_back({_importPacket, _remote->Selected->Ready});
}

void SceneParticles::Submitted(const std::vector<CompletionToken>& completions)
{
	if (_importPacket < completions.size() && completions[_importPacket])
	{
		auto& remote = *_remote;
		remote.Selected->Reader = completions[_importPacket];
		remote.CachedVersion = remote.Selected->Version;
		++remote.Stats.Imports;
	}
}

void SceneParticles::Presented(uint64_t frame)
{
	if (!_remote) return;
	auto& remote = *_remote;
	const auto* selected = remote.Selected;
	const auto previous = remote.PresentedVersion;
	remote.PresentedVersion = selected ? selected->Version : 0;
	if (selected) remote.PresentedSnapshot = selected->Snapshot;
	auto& stats = remote.Stats;
	++stats.Frames;
	if (!selected) ++stats.Empty;
	else if (remote.PresentedVersion == previous) ++stats.Repeats;
	else if (remote.PresentedVersion > previous + 1) stats.Skipped += remote.PresentedVersion - previous - 1;
	if (_sampleVisibility && selected)
	{
		auto& sample = _visibilitySamples.at(_resources.CurrFrameConstantsIndex);
		sample.Frame = frame;
		sample.Version = selected->Version;
	}
	if (stats.Frames <= 12 && remote.Ordered)
		Log::Info("RenderGraph", "Particles MGPU frame=", frame, " CrossSync=", remote.Mode,
		    " generation=", remote.Generation, " ordered=", remote.Ordered, " exported=", remote.LastOrder ? remote.LastOrder->Version : 0,
		    " shown=", remote.PresentedVersion, " exportFence=", selected ? selected->Ready.Value : 0,
		    " importFence=", _importPacket != Invalid && selected ? selected->Reader.Value : 0,
		    " readyAtChoice=", remote.LatestReady() && selected, " gameSeconds=", remote.GameTime);
	if (LogSnapshots.GetValue() && stats.Frames == 24)
		Log::Info("RenderGraph", "Particles MGPU summary frame=", frame, " CrossSync=", remote.Mode,
		    " generation=", remote.Generation, " frames=", stats.Frames, " steps=", remote.Ordered,
		    " publications=", stats.Exports, " imports=", stats.Imports, " repeats=", stats.Repeats,
		    " skippedShows=", stats.Skipped, " noSnapshotFrames=", stats.Empty,
		    " inputRetirementWaits=", stats.InputWaits, " outputRetirementWaits=", stats.OutputWaits,
		    " gameSeconds=", remote.GameTime);
}
void SceneParticles::AppendSimulation(Graph& graph, std::vector<ID3D12Resource*>& bindings, Inputs inputs,
                                      GDX12Texture* depth)
{
	const auto& ids = _groups[size_t(inputs)];
	auto* simulation = _resources.ParticleSimulation.get();
	std::vector<Access> pools;
	AddResources(graph, bindings, pools, simulation->GetPoolResources(&ids), Mode::ReadWrite);
	if (pools.empty())
	{
		return;
	}
	const bool async = AsyncSimulation.GetValue();
	const Queue target = async ? Queue::Compute : Queue::Direct;
	const State simulationRead = async ? State(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) : State(-1);
	if (async)
	{
		for (auto& access : pools)
		{
			access.Required = UnorderedAccess;
		}
	}
	const char* initialize[] = {"particle frame pool initialization/reset", "particle interaction pool initialization/reset",
	                            "particle depth pool initialization/reset"};
	const char* simulate[] = {"particle simulation from frame inputs", "particle simulation from interaction field",
	                          "particle simulation from depth prepass"};
	Packet(graph, bindings, initialize[size_t(inputs)], pools,
	       [this, simulation, ids](auto* cmd) { simulation->InitializePools(cmd, &_resources, &ids); }, target);
	const UINT slot = _resources.CurrFrameConstantsIndex;
	AddResources(graph, bindings, pools, simulation->GetConstantResources(slot, &ids), Mode::Read);
	pools.push_back(ResourceAccess(graph, bindings, &_resources.FrameConstants.at(slot)->CameraCB->GetResource(), Mode::Read));
	if (inputs == Inputs::Depth)
	{
		pools.push_back(ResourceAccess(graph, bindings, depth->GetResource(), Mode::Read, simulationRead));
	}
	if (inputs != Inputs::Frame && _interaction.Resource)
	{
		pools.push_back(ResourceAccess(graph, bindings, _interaction.Resource, Mode::Read, simulationRead));
	}
	Packet(graph, bindings, simulate[size_t(inputs)], std::move(pools), [this, simulation, depth, ids](auto* cmd)
	       { simulation->Execute(cmd, &_resources, _common, depth, &ids); }, target);
}
void SceneParticles::PrepareOpaque(const SceneGeometry& geometry, UINT slot)
{
	auto* depth = geometry.Depth(slot);
	if (_width && (_width != depth->GetWidth() || _height != depth->GetHeight()))
	{
		_pass.Resize();
	}
	_width = depth->GetWidth();
	_height = depth->GetHeight();
	_pass.SetMotionOutputEnabled(HasDrawSources(false) &&
	    (GetMotionBlurSettings().Enabled || GetFrameGenerationProvider() != GDX12FrameGenerationProvider::Off ||
	     (_common.Upscaler && (_common.Upscaler->GetFlags() & RENDER_PASS_FLAG_USE_JITTER))));
	_pass.BeginFrame(depth, _renderFrame);
	_opaqueReady = _opaquePrepared = false;
	_splitOpaque = geometry.PrepassNormals(slot) != nullptr;
	if (!HasDrawSources(true)) return;
	std::array<GDX12Texture*, 5> gbuffer;
	for (UINT i = 0; i < gbuffer.size(); ++i) gbuffer[i] = geometry.GBuffer(slot, i);
	_opaqueReady = _pass.InitializeOpaque(gbuffer, _splitOpaque);
}

void SceneParticles::AppendEarlyOpaquePreparation(Graph& graph, std::vector<ID3D12Resource*>& bindings)
{
	if (!_opaqueReady || !_splitOpaque) return;
	const bool needsDepth = std::any_of(_renderFrame.Sources.begin(), _renderFrame.Sources.end(), [](const auto& source) {
		return source.Emitter.Blend == ParticleBlend::Opaque && source.Emitter.DepthCollision;
	});
	if (!needsDepth)
	{
		AppendPreparation(graph, bindings, true);
		_opaquePrepared = true;
	}
}

void SceneParticles::AppendOpaquePrepass(Graph& graph, std::vector<ID3D12Resource*>& bindings,
                                        const SceneGeometry& geometry, UINT slot)
{
	if (!_opaqueReady || !_splitOpaque) return;
	if (!_opaquePrepared) AppendPreparation(graph, bindings, true);
	AppendOpaqueDraw(graph, bindings, geometry, slot, GDX12ParticlesPass::OpaqueStage::DepthNormals);
}

void SceneParticles::AppendOpaque(Graph& graph, std::vector<ID3D12Resource*>& bindings, const SceneGeometry& geometry,
                                  UINT slot)
{
	if (!_opaqueReady) return;
	if (!_splitOpaque) AppendPreparation(graph, bindings, true);
	using Stage = GDX12ParticlesPass::OpaqueStage;
	AppendOpaqueDraw(graph, bindings, geometry, slot, _splitOpaque ? Stage::GBuffer : Stage::Combined);
}

void SceneParticles::AppendOpaqueDraw(Graph& graph, std::vector<ID3D12Resource*>& bindings,
                                      const SceneGeometry& geometry, UINT slot, GDX12ParticlesPass::OpaqueStage stage)
{
	using Stage = GDX12ParticlesPass::OpaqueStage;
	auto* depth = geometry.Depth(slot);
	const bool normals = stage == Stage::DepthNormals;
	const bool readOnly = stage == Stage::GBuffer;
	const UINT count = normals ? 1 : 5;
	std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 5> targets{};
	auto accesses = DrawReads(graph, bindings, true);
	AddAccesses(graph, bindings, accesses, _pass.NativeRasterAccesses(false));
	for (UINT i = 0; i < count; ++i)
	{
		auto* target = normals ? geometry.PrepassNormals(slot) : geometry.GBuffer(slot, i);
		targets[i] = target->GetRTV()->CPUHandle;
		accesses.push_back(ResourceAccess(graph, bindings, target->GetResource(), Mode::ReadWrite,
		                                  D3D12_RESOURCE_STATE_RENDER_TARGET));
	}
	accesses.push_back(ResourceAccess(graph, bindings, depth->GetResource(), readOnly ? Mode::Read : Mode::ReadWrite,
	                                  readOnly ? D3D12_RESOURCE_STATE_DEPTH_READ : D3D12_RESOURCE_STATE_DEPTH_WRITE));
	const auto dsv = readOnly ? geometry.ReadOnlyDepthDSV(slot) : geometry.DepthDSV(slot);
	const char* name = normals ? "opaque particles DepthNormals" :
	                   readOnly ? "opaque particles G-buffer (read-only depth)" : "opaque particles G-buffer and depth";
	Packet(graph, bindings, name, std::move(accesses),
	       [this, depth, dsv, targets, count, stage](auto* cmd) { _pass.DrawOpaque(cmd, depth, dsv, targets.data(), count, stage); });
}
bool SceneParticles::HasDrawSources(bool opaque) const
{
	return std::any_of(_renderFrame.Sources.begin(), _renderFrame.Sources.end(), [opaque](const auto& source)
	                   { return (source.Emitter.Blend == ParticleBlend::Opaque) == opaque; });
}
void SceneParticles::AppendPreparation(Graph& graph, std::vector<ID3D12Resource*>& bindings, bool opaque)
{
	const UINT slot = _resources.CurrFrameConstantsIndex;
	const auto camera =
	    ResourceAccess(graph, bindings, &_resources.FrameConstants.at(slot)->CameraCB->GetResource(), Mode::Read);
	std::vector<Access> preparation{camera};
	AddAccesses(graph, bindings, preparation, _pass.NativePreparationAccesses(_renderFrame, opaque));
	GDX12Resource* readback = nullptr;
	if (!opaque && _remote && _remote->Selected && LogSnapshots.GetValue() && !_visibilityConfirmed)
	{
		auto& buffer = _visibilitySamples.at(slot).Buffer;
		if (!buffer.D3DResource)
			buffer.D3DResource = _resources.Device->CreateResource(D3D12_HEAP_TYPE_READBACK,
			    CD3DX12_RESOURCE_DESC::Buffer(2 * sizeof(UINT)), D3D12_RESOURCE_STATE_COPY_DEST);
		preparation.push_back(ResourceAccess(graph, bindings, &buffer, Mode::Write, CopyDest));
		readback = &buffer;
		_sampleVisibility = true;
	}
	Packet(graph, bindings, opaque ? "opaque particles frustum cull" : "transparent particles cull and global sort",
	       std::move(preparation), [this, opaque, readback](auto* cmd) {
		       _pass.PrepareRenderData(cmd, _renderFrame, opaque, readback, _firstRemote);
	       });
}
std::vector<Access> SceneParticles::DrawReads(Graph& graph, std::vector<ID3D12Resource*>& bindings, bool opaque)
{
	const UINT slot = _resources.CurrFrameConstantsIndex;
	std::vector<Access> reads{
	    ResourceAccess(graph, bindings, &_resources.FrameConstants.at(slot)->CameraCB->GetResource(), Mode::Read)};
	for (const auto& source : _renderFrame.Sources)
	{
		const auto& emitter = source.Emitter;
		if ((emitter.Blend == ParticleBlend::Opaque) == opaque && emitter.Texture)
		{
			auto* atlas = emitter.Texture->PrimaryDeviceTexture;
			if (!atlas)
			{
				throw std::logic_error("Particle atlas has no primary texture");
			}
			constexpr State published = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
			const Id id = NativeWork::ImportReadOnlyTexture(graph, bindings, *atlas->GetResource(), published,
			                                                "renderer particle atlas");
			reads.push_back({id, Mode::Read, published});
		}
	}
	return reads;
}
void SceneParticles::Prepare(Graph& graph, std::vector<ID3D12Resource*>& bindings, GDX12Texture* rasterDepth,
                             GDX12Texture* accum, GDX12Texture* reveal)
{
	_drawReads.clear();
	if (!HasDrawSources(false) || _pass.NativeRasterAccesses(false).empty())
	{
		return;
	}
	_accum = accum;
	_reveal = reveal;
	_pass.SetInputs({rasterDepth, accum, reveal});
	_pass.Initialize();
	if (_remote) _import = _remote->AppendImport(_work, graph, bindings);
	AppendPreparation(graph, bindings, false);
	_drawReads = DrawReads(graph, bindings, false);
	_drawReads.push_back(ResourceAccess(graph, bindings, rasterDepth->GetResource(), Mode::Read));
}
void SceneParticles::AppendWeighted(Graph& graph, std::vector<ID3D12Resource*>& bindings)
{
	if (_drawReads.empty()) return;
	auto accesses = _drawReads;
	AddAccesses(graph, bindings, accesses, _pass.NativeRasterAccesses(true));
	accesses.push_back(
	    ResourceAccess(graph, bindings, _accum->GetResource(), Mode::ReadWrite, D3D12_RESOURCE_STATE_RENDER_TARGET));
	accesses.push_back(
	    ResourceAccess(graph, bindings, _reveal->GetResource(), Mode::ReadWrite, D3D12_RESOURCE_STATE_RENDER_TARGET));
	Packet(graph, bindings, "particle weighted-alpha append", std::move(accesses),
	       [this](auto* cmd) { _pass.DrawWeightedAndMotion(cmd); });
}
void SceneParticles::AppendComposite(Graph& graph, std::vector<ID3D12Resource*>& bindings, GDX12Texture* hdr)
{
	if (_drawReads.empty()) return;
	auto accesses = _drawReads;
	AddAccesses(graph, bindings, accesses, _pass.NativeRasterAccesses(false));
	accesses.push_back(
	    ResourceAccess(graph, bindings, hdr->GetResource(), Mode::ReadWrite, D3D12_RESOURCE_STATE_RENDER_TARGET));
	Packet(graph, bindings, "global SortedAlpha/additive HDR particles", std::move(accesses),
	       [this, hdr](auto* cmd) { _pass.Composite(cmd, hdr); });
}
} // namespace BistroRender

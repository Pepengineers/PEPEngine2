#include "SceneWater.h"
#include "Common/ConsoleVariables.h"
#include "Engine.Core/BenchmarkEngine.h"
#include "Engine.Core/Logging/Logger.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12TextureResource.h"
#include <chrono>
#include <condition_variable>
#include <thread>

namespace BistroRender
{
namespace
{
AutoConsoleVariable<bool> AsyncWater(L"App.Async.Water", false,
                                     L"Run the SPH water simulation on the local COMPUTE queue.",
                                     EConsoleVariableFlags::Default);
AutoConsoleVariable<int> WaterAdapter(L"App.Water.Adapter", -1,
    L"Secondary DXGI index for Bistro SPH. -1 keeps the solver on GPU0.",
    EConsoleVariableFlags::Default, [](int value) { return value >= -1; });
AutoConsoleVariable<int> CrossSync(L"App.Water.CrossSync", 0,
    L"Remote water: 0 exact previous (first uses current), 1 exact current, 2 independent GPU1 and latest completed (first waits for export).",
    EConsoleVariableFlags::Default, [](int value) { return value >= 0 && value <= 2; });
constexpr State CopyRead = D3D12_RESOURCE_STATE_COPY_SOURCE;
constexpr State CopyWrite = D3D12_RESOURCE_STATE_COPY_DEST;
using WaterClock = std::chrono::steady_clock;
using WaterEvent = std::unique_ptr<void, decltype(&CloseHandle)>;

WaterEvent MakeEvent()
{
	WaterEvent event(CreateEventW(nullptr, FALSE, FALSE, nullptr), CloseHandle);
	if (!event) ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
	return event;
}
}

struct SceneWater::Remote
{
	struct Range
	{
		uint64_t Id, Offset;
		UINT Count;
	};
	struct Snapshot
	{
		QueueSubmission::TransferBuffer Shared;
		std::vector<Range> Ranges;
		std::vector<GDX12FluidInstance> Instances;
		GDX12FluidControls Controls{};
		uint64_t Bytes = 0, Generation = 0, Version = 0;
		CompletionToken Ready, Reader;
		bool Writing = false;
	};
	struct ClockState { double Time = 0; float Rate = 0; };

	std::shared_ptr<QueueSubmission> Submit;
	GDX12DeviceResources Secondary;
	GDX12FluidParticlePass Pass;
	Executor GPU1;
	std::vector<Snapshot> Ring;
	NativeWork Work;
	Snapshot *Selected = nullptr, *LastOrder = nullptr;
	size_t Next = 0;
	uint64_t Ordered = 0, Generation = 0, TotalSubsteps = 0;
	uint64_t DisplayFrames = 0, Imports = 0, Repeats = 0, Skipped = 0, LastShown = 0;
	double SimulatedSeconds = 0;
	bool WaitForSelected = true;
	int Mode = 1;
	using Seed = std::pair<std::shared_ptr<const std::vector<GDX12FluidParticle>>, uint64_t>;
	std::map<uint64_t, Seed> Seeds;
	std::mutex Mutex;
	std::condition_variable Published;
	WaterEvent Wake{nullptr, CloseHandle}, Completed{nullptr, CloseHandle};
	std::thread Worker;
	std::exception_ptr WorkerFailure, DrainFailure;
	bool Stopping = false, Suspended = false;
	std::vector<GDX12FluidInstance> Inputs;
	GDX12FluidControls InputControls{};
	std::map<uint64_t, ClockState> Clocks;
	WaterClock::time_point ClockAnchor{};
	uint64_t Revision = 0;

	Remote(GDX12DeviceResources& primary, const std::shared_ptr<QueueSubmission>& submit)
	    : Submit(submit), Pass(Secondary), GPU1(submit, 1, UINT(primary.FrameConstants.size()), 1),
	      Ring(primary.FrameConstants.size() + 1)
	{
		Secondary.Device = &Submit->Device(1);
		Secondary.FrameConstants.resize(primary.FrameConstants.size()); // Only the upload slot count is needed.
		Secondary.SRV_UAV_Heap = std::make_shared<GDX12DescriptorHeap>(Secondary.Device,
		    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, TextureResources_StartIndex + TextureResources_RangeLength,
		    D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
		Pass.SetResourceRetirement([this] { GPU1.Drain(); });
		const auto gpu = Secondary.Device->GetAdapterLuid();
		Log::Info("RenderGraph", "Water MGPU solver GPU1 LUID=", gpu.HighPart, ":", gpu.LowPart);
	}

	void Configure(const std::vector<GDX12FluidInstance>& instances, GDX12FluidControls controls, int mode)
	{
		std::map<uint64_t, Seed> seeds;
		if (controls.Enabled)
			for (const auto& instance : instances)
			{
				const auto& settings = instance.Component.GetSettings();
				const auto& seed = instance.Component.GetParticles();
				if (seed && !seed->empty() && (settings.RenderEnabled || settings.SimulationEnabled))
					seeds.emplace(instance.Id, Seed{seed, instance.Component.GetResetRevision()});
			}
		if (Mode != mode || Seeds != seeds)
		{
			Seeds = std::move(seeds);
			Mode = mode;
			LastOrder = nullptr;
			Ordered = TotalSubsteps = DisplayFrames = Imports = Repeats = Skipped = LastShown = 0;
			SimulatedSeconds = 0;
			++Generation;
		}
		Selected = nullptr;
	}

	bool PrepareSolve(Executor& executor, const std::vector<GDX12FluidInstance>& instances,
	                  GDX12FluidControls controls, UINT frameSlot)
	{
		executor.Begin(frameSlot);
		Secondary.CurrFrameConstantsIndex = frameSlot;
		Pass.PrepareFrame(instances, controls, GDX12FluidParticlePass::Role::Solver);
		return Pass.NeedsSimulation();
	}

	Snapshot* PrepareFrameOrder(const std::vector<GDX12FluidInstance>& instances, GDX12FluidControls controls, UINT frameSlot, int mode)
	{
		StopWorker();
		Configure(instances, controls, mode);
		WaitForSelected = true;
		if (!PrepareSolve(GPU1, instances, controls, frameSlot)) return nullptr;
		if (Mode == 0) Selected = LastOrder;
		auto* output = &Ring[Next++ % Ring.size()];
		// The previous order stays pinned through selection, even before a new read is submitted.
		if (output == Selected) output = &Ring[Next++ % Ring.size()];
		auto& slot = *output;
		if (slot.Ready) Submit->Wait(slot.Ready);
		if (slot.Reader) Submit->Wait(slot.Reader);
		PrepareSnapshot(slot, instances, controls);
		return &slot;
	}

	void PrepareSnapshot(Snapshot& slot, const std::vector<GDX12FluidInstance>& instances, GDX12FluidControls controls)
	{
		slot.Reader = {};
		slot.Ranges.clear();
		slot.Instances = instances;
		slot.Controls = controls;
		uint64_t bytes = 0;
		for (const auto& source : Pass.DrawSources())
		{
			const UINT count = source.Particles->GetElementCount();
			slot.Ranges.push_back({source.Instance, bytes, count});
			bytes += uint64_t(count) * sizeof(XMFLOAT4);
		}
		if (bytes > slot.Bytes)
		{
			slot.Shared = Submit->CreateTransferBuffer(bytes);
			slot.Bytes = bytes;
		}
	}

	CompletionToken SubmitSnapshot(Executor& executor, Snapshot& slot)
	{
		Work.Commands.clear();
		Graph graph;
		graph.Device = 1;
		graph.FrameBoundary = false;
		graph.TerminalQueue = Queue::Compute;
		std::vector<ID3D12Resource*> bindings;
		Packet(Work, graph, bindings, "GPU1 water seed / SPH hash-sort-density-integrate", Pass.GetSimulationBufferAccesses(),
		    [this](auto* cmd) { Pass.ExecuteSimulation(cmd); }, Queue::Compute);
		if (!slot.Ranges.empty())
		{
			Packet(Work, graph, bindings, "GPU1 pack water position and speed", Pass.GetPackAccesses(),
			    [this](auto* cmd) { Pass.PackDrawSources(cmd); }, Queue::Compute);
			auto* shared = slot.Shared.Resources[1].Get();
			std::vector<Access> accesses{{NativeWork::ResourceFor(graph, bindings, shared, "shared water display"), Mode::Write, CopyWrite}};
			for (const auto& source : Pass.DrawSources())
				accesses.push_back({NativeWork::ResourceFor(graph, bindings, source.Particles->GetResource().D3DResource.Get(),
				    "water packed display"), Mode::Read, CopyRead});
			Work.Add(graph, "GPU1 export water display", std::move(accesses), [this, shared, &slot](auto* cmd) {
				const auto& sources = Pass.DrawSources();
				for (size_t i = 0; i < sources.size(); ++i)
				{
					const auto& range = slot.Ranges[i];
					cmd->CopyBufferRegion(shared, range.Offset, sources[i].Particles->GetResource().D3DResource.Get(),
					    0, uint64_t(range.Count) * sizeof(XMFLOAT4));
				}
			}, {}, Queue::Compute);
		}
		const auto schedule = Compile(graph);
		executor.Reserve(schedule);
		executor.Execute(schedule, bindings, [this](Id op, uint32_t, auto* cmd) { Work.Record(op, cmd); });
		return executor.Completions().back();
	}

	void Commit(Snapshot& slot, CompletionToken ready, uint64_t generation)
	{
		slot.Ready = ready;
		slot.Writing = false;
		slot.Generation = generation;
		if (generation != Generation) return;
		slot.Version = ++Ordered;
		TotalSubsteps += Pass.Substeps();
		SimulatedSeconds += Pass.SimulatedSeconds();
		LastOrder = &slot;
	}

	void PublishFrameOrder(Snapshot& slot)
	{
		Commit(slot, SubmitSnapshot(GPU1, slot), Generation);
		if (!Selected) Selected = LastOrder;
	}

	Snapshot* NewestCompleted()
	{
		Snapshot* latest = nullptr;
		for (auto& slot : Ring)
			if (!slot.Writing && slot.Generation == Generation && slot.Version &&
			    (!latest || latest->Version < slot.Version) && Submit->Complete(slot.Ready))
				latest = &slot;
		return latest;
	}

	void AdvanceClocks(WaterClock::time_point now)
	{
		const double elapsed = std::chrono::duration<double>(now - ClockAnchor).count();
		for (auto& [id, clock] : Clocks) clock.Time += elapsed * clock.Rate;
		ClockAnchor = now;
	}

	void PauseClock()
	{
		if (!Worker.joinable()) return;
		std::lock_guard lock(Mutex);
		AdvanceClocks(WaterClock::now());
		for (auto& [id, clock] : Clocks) clock.Rate = 0;
		Suspended = true;
		SetEvent(Wake.get());
	}

	void RunIndependent(Executor& executor)
	{
		CompletionToken pending, armed;
		uint64_t generation = 0, revision = 0;
		std::map<uint64_t, double> sampled;
		for (;;)
		{
			std::unique_lock lock(Mutex);
			if (Stopping) return;
			CompletionToken wait;
			Snapshot* output = nullptr;
			const bool advancing = !Suspended && std::any_of(Clocks.begin(), Clocks.end(),
			    [](const auto& entry) { return entry.second.Rate > 0; });
			const bool changed = generation != Generation || revision != Revision;
			const bool retireOnly = Seeds.empty() && changed;
			if (pending && !Submit->Complete(pending)) wait = pending;
			else if (!Seeds.empty() && (changed || advancing))
			{
				const auto* latest = NewestCompleted();
				for (auto& slot : Ring)
				{
					if (&slot == Selected || &slot == latest) continue;
					if (slot.Ready && !Submit->Complete(slot.Ready)) wait = slot.Ready;
					else if (slot.Reader && !Submit->Complete(slot.Reader)) wait = slot.Reader;
					else { output = &slot; break; }
				}
			}
			if (!output && (wait || !retireOnly))
			{
				lock.unlock();
				if (wait && (wait.Device != armed.Device || wait.Target != armed.Target || wait.Value != armed.Value))
				{
					Submit->NotifyOnCompletion(wait, Completed.get());
					armed = wait;
				}
				const HANDLE events[]{Wake.get(), Completed.get()};
				const auto result = WaitForMultipleObjects(2, events, FALSE, wait ? 100 : INFINITE);
				if (result == WAIT_FAILED) ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
				if (result == WAIT_TIMEOUT) Submit->CheckHealth();
				continue;
			}
			AdvanceClocks(WaterClock::now());
			if (generation != Generation) sampled.clear();
			generation = Generation;
			revision = Revision;
			auto instances = Inputs;
			const auto controls = InputControls;
			for (auto& instance : instances)
			{
				const auto& clock = Clocks.at(instance.Id);
				const auto previous = sampled.try_emplace(instance.Id, clock.Time).first;
				instance.DeltaSeconds = clock.Rate > 0 ? float(clock.Time - previous->second) : 0;
				previous->second = clock.Time;
			}
			if (output) output->Writing = true;
			lock.unlock();
			if (!PrepareSolve(executor, instances, controls, 0) && output)
				throw std::logic_error("Active remote water has no solver work");
			if (!output) continue;
			PrepareSnapshot(*output, instances, controls);
			pending = SubmitSnapshot(executor, *output);
			lock.lock();
			Commit(*output, pending, generation);
			Published.notify_all();
		}
	}

	void StartWorker()
	{
		if (Worker.joinable()) return;
		GPU1.Drain();
		if (!Wake) Wake = MakeEvent();
		if (!Completed) Completed = MakeEvent();
		Worker = std::thread([this] {
			const auto fail = [this](bool drain) {
				std::lock_guard lock(Mutex);
				WorkerFailure = std::current_exception();
				if (drain) DrainFailure = WorkerFailure;
				Published.notify_all();
			};
			try
			{
				Executor executor(Submit, 1, 1, 1);
				Pass.SetResourceRetirement([&executor] { executor.Drain(); });
				try { RunIndependent(executor); }
				catch (...) { fail(false); }
				try { executor.Drain(); }
				catch (...) { fail(true); }
			}
			catch (...) { fail(false); }
		});
	}

	void StopWorker()
	{
		if (Worker.joinable())
		{
			{
				std::lock_guard lock(Mutex);
				Stopping = true;
				SetEvent(Wake.get());
			}
			Worker.join();
			Pass.SetResourceRetirement([this] { GPU1.Drain(); });
			Stopping = Suspended = false;
			WorkerFailure = {};
			Inputs.clear();
			Clocks.clear();
		}
		if (DrainFailure) std::rethrow_exception(DrainFailure);
	}

	void Drain()
	{
		StopWorker();
		GPU1.Drain();
	}

	void PrepareIndependent(const std::vector<GDX12FluidInstance>& instances, GDX12FluidControls controls)
	{
		std::unique_lock lock(Mutex);
		Configure(instances, controls, 2);
		const bool changed = controls.Enabled != InputControls.Enabled || controls.Quality != InputControls.Quality ||
		    !std::equal(instances.begin(), instances.end(), Inputs.begin(), Inputs.end(), [](const auto& a, const auto& b) {
			    return a.Id == b.Id && a.Component.GetSettingsRevision() == b.Component.GetSettingsRevision() &&
			        a.ContainerToWorld == b.ContainerToWorld;
		    });
		AdvanceClocks(WaterClock::now());
		std::map<uint64_t, ClockState> clocks;
		for (const auto& instance : instances)
		{
			auto clock = Clocks[instance.Id];
			const auto& settings = instance.Component.GetSettings();
			clock.Rate = controls.Enabled && settings.SimulationEnabled && settings.TimeScale > 0 &&
			    Seeds.count(instance.Id) ? instance.ClockScale : 0;
			clocks.emplace(instance.Id, clock);
		}
		Clocks = std::move(clocks);
		Suspended = false;
		Inputs = instances;
		InputControls = controls;
		if (changed) ++Revision;
		if (!Worker.joinable() && Seeds.empty()) return;
		if (!Worker.joinable())
			PrepareSolve(GPU1, instances, controls, 0); // Build the solver pipelines before the worker takes ownership.
		StartWorker();
		SetEvent(Wake.get());
		const bool visible = std::any_of(instances.begin(), instances.end(), [&](const auto& instance) {
			return Seeds.count(instance.Id) && instance.Component.GetSettings().RenderEnabled;
		});
		if (visible)
		{
			const bool bootstrap = !LastOrder;
			Published.wait(lock, [&] { return LastOrder || WorkerFailure; });
			if (WorkerFailure) std::rethrow_exception(WorkerFailure);
			Selected = NewestCompleted();
			if (!Selected) Selected = LastOrder;
			WaitForSelected = bootstrap || !Submit->Complete(Selected->Ready);
		}
		else if (WorkerFailure) std::rethrow_exception(WorkerFailure);
	}

	std::vector<GDX12FluidInstance> DisplayInstances(const std::vector<GDX12FluidInstance>& current) const
	{
		auto instances = Selected->Instances;
		for (auto& instance : instances)
		{
			const auto found = std::find_if(current.begin(), current.end(), [&](const auto& value) {
				return value.Id == instance.Id;
			});
			const bool render = found != current.end() && found->Component.GetSettings().RenderEnabled &&
			    std::any_of(Selected->Ranges.begin(), Selected->Ranges.end(), [&](const auto& range) {
				    return range.Id == instance.Id;
			    });
			auto settings = instance.Component.GetSettings();
			if (settings.RenderEnabled == render) continue;
			settings.RenderEnabled = render;
			instance.Component.SetSettings(settings);
		}
		return instances;
	}
};

SceneWater::SceneWater(GDX12DeviceResources& resources, RenderPipelineCommonData& common)
    : _resources(resources),
      _collector(BenchmarkEngine::GetLocator().GetModule<SceneManagerModule>()), _surface(resources, &common)
{
}
SceneWater::~SceneWater()
{
	if (!_remote) return;
	try { _remote->Drain(); }
	catch (...) { (void)_remote.release(); } // A quarantined queue can still own these resources.
}
int SceneWater::SecondaryAdapter() { return WaterAdapter.GetValue(); }
void SceneWater::PauseIndependentClock()
{
	if (_remote) _remote->PauseClock();
}
void SceneWater::SetRetirement(std::function<void()> retire)
{
	_surface.SetResourceRetirement(std::move(retire));
}
void SceneWater::RetireRemote(Executor& primary)
{
	if (!_remote) return;
	primary.Drain();
	_remote->Drain();
	_remote.reset();
}
void SceneWater::Prepare(float delta, Executor& primary, int activeAdapter)
{
	if (!_surface.HasRetirement())
	{
		throw std::logic_error("Water requires executor-owned resource retirement");
	}
	_work.Commands.clear();
	_import = _draw = _importPacket = _drawPacket = Invalid;
	auto instances = _collector.CollectScene(delta);
	const auto controls = GetFluidControls();
	const auto& submit = primary.Submissions();
	const uint64_t domain = activeAdapter >= 0 && activeAdapter == SecondaryAdapter() ? submit->Incarnation() : 0;
	if (_attemptedDomain != domain)
	{
		RetireRemote(primary);
		_attemptedDomain = domain;
		_failedDomain = 0;
	}
	// Visibility and simulation flags can leave a live instance temporarily idle.
	if (instances.empty()) RetireRemote(primary);
	Remote::Snapshot* slot = nullptr;
	try
	{
		if (domain && !_remote && _failedDomain != domain && controls.Enabled && !instances.empty())
			_remote = std::make_unique<Remote>(_resources, submit);
		if (_remote)
		{
			const int mode = CrossSync.GetValue();
			if (mode == 2) _remote->PrepareIndependent(instances, controls);
			else slot = _remote->PrepareFrameOrder(instances, controls, _resources.CurrFrameConstantsIndex, mode);
		}
	}
	catch (const DxException& e)
	{
		RetireRemote(primary);
		_failedDomain = domain;
		Log::Warning("RenderGraph", "Local water fallback: ", e.ToString());
	}
	catch (const std::exception& e)
	{
		RetireRemote(primary);
		_failedDomain = domain;
		Log::Warning("RenderGraph", "Local water fallback: ", e.what());
	}
	if (slot) _remote->PublishFrameOrder(*slot);
	if (_remote && _remote->Selected)
		_surface.PrepareFrame(_remote->DisplayInstances(instances), _remote->Selected->Controls,
		    GDX12FluidParticlePass::Role::Display);
	else
		_surface.PrepareFrame(std::move(instances), _remote ? GDX12FluidControls{false, controls.Quality} : controls,
		    _remote ? GDX12FluidParticlePass::Role::Display : GDX12FluidParticlePass::Role::Local);
}
Id SceneWater::Packet(NativeWork& work, Graph& graph, std::vector<ID3D12Resource*>& bindings, const char* name,
                        const std::vector<GDX12ResourceAccess>& resources,
                        std::function<void(GDX12CommandList*)> record, Queue target)
{
	std::vector<Access> accesses;
	for (const auto& resource : resources)
	{
		auto* native = resource.Resource->D3DResource.Get();
		const Id id = NativeWork::ResourceFor(graph, bindings, native, name);
		const bool upload = graph.Resources.at(id).Memory == Heap::Upload;
		const auto mode = upload || resource.Usage == GDX12AccessMode::Read ? Mode::Read
		                  : resource.Usage == GDX12AccessMode::Write        ? Mode::Write
		                                                                    : Mode::ReadWrite;
		const State state =
		    upload                                        ? State(D3D12_RESOURCE_STATE_GENERIC_READ)
		    : resource.State != D3D12_RESOURCE_STATES(-1) ? State(resource.State)
		    : native->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER
		        ? State(mode == Mode::Read ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
		        : State(D3D12_RESOURCE_STATE_COMMON);
		const auto found =
		    std::find_if(accesses.begin(), accesses.end(), [id](auto access) { return access.Resource == id; });
		if (found == accesses.end())
		{
			accesses.push_back({id, mode, state});
		}
		else
		{
			if (found->Required != state)
			{
				throw std::logic_error("Conflicting water packet states");
			}
			if (found->Usage != mode)
			{
				found->Usage = Mode::ReadWrite;
			}
		}
	}
	return work.AddOwned(graph, name, std::move(accesses), bindings, std::move(record),
	               "Water recording cannot submit private queue signals", {}, target);
}
void SceneWater::SetSceneInputs(GDX12Texture* opaqueDepth, GDX12Texture* opaqueMotion)
{
	if (!opaqueDepth->GetWidth() || !opaqueDepth->GetHeight() || opaqueDepth->GetWidth() != opaqueMotion->GetWidth() ||
	    opaqueDepth->GetHeight() != opaqueMotion->GetHeight() || opaqueMotion->GetFormat() != DXGI_FORMAT_R16G16_FLOAT)
	{
		throw std::invalid_argument("Fluid surface requires matching reverse-Z depth and RG16F motion grids");
	}
	_depth = opaqueDepth;
	_motion = opaqueMotion;
}
void SceneWater::AppendSimulation(Graph& graph, std::vector<ID3D12Resource*>& bindings)
{
	if (_remote) return;
	const Queue queue = AsyncWater.GetValue() ? Queue::Compute : Queue::Direct;
	if (_surface.NeedsSimulation())
	{
		Packet(_work, graph, bindings, "water seed / SPH hash-sort-density-integrate", _surface.GetSimulationBufferAccesses(),
		       [this](auto* commands) { _surface.ExecuteSimulation(commands); }, queue);
	}
	if (!_surface.DrawSources().empty())
	{
		Packet(_work, graph, bindings, "water pack position and speed", _surface.GetPackAccesses(),
		       [this](auto* commands) { _surface.PackDrawSources(commands); }, queue);
	}
}
void SceneWater::AppendDraw(Graph& graph, std::vector<ID3D12Resource*>& bindings, GDX12Texture* hdr,
                            D3D12_CPU_DESCRIPTOR_HANDLE depth)
{
	const auto& sources = _surface.DrawSources();
	if (sources.empty())
	{
		return;
	}
	if (_remote)
	{
		const auto& snapshot = *_remote->Selected;
		auto* shared = snapshot.Shared.Resources[0].Get();
		std::vector<Access> copies{{NativeWork::ResourceFor(graph, bindings, shared, "shared water display"), Mode::Read, CopyRead}};
		std::vector<std::pair<ID3D12Resource*, Remote::Range>> targets;
		for (const auto& source : sources)
		{
			const auto range = std::find_if(snapshot.Ranges.begin(), snapshot.Ranges.end(), [&](const auto& item) {
				return item.Id == source.Instance;
			});
			if (range == snapshot.Ranges.end() || range->Count != source.Particles->GetElementCount())
				throw std::logic_error("Water snapshot and draw source disagree");
			auto* target = source.Particles->GetResource().D3DResource.Get();
			copies.push_back({NativeWork::ResourceFor(graph, bindings, target, "local water display"), Mode::Write, CopyWrite});
			targets.push_back({target, *range});
		}
		_import = _work.Add(graph, "GPU0 import water display", std::move(copies), [shared, targets = std::move(targets)](auto* cmd) {
			for (const auto& [target, range] : targets)
				cmd->CopyBufferRegion(target, 0, shared, range.Offset, uint64_t(range.Count) * sizeof(XMFLOAT4));
		}, {}, Queue::Copy);
	}
	auto accesses = _surface.NativeSurfaceAccesses(sources);
	accesses.push_back({hdr->GetResource(), GDX12AccessMode::ReadWrite, D3D12_RESOURCE_STATE_RENDER_TARGET});
	accesses.push_back({_depth->GetResource(), GDX12AccessMode::ReadWrite, D3D12_RESOURCE_STATE_DEPTH_WRITE});
	_draw = Packet(_work, graph, bindings, "water draw unlit opaque particles", accesses,
	       [this, hdr, depth, sources](auto* commands) { _surface.DrawParticles(commands, hdr, depth, sources); });
}

void SceneWater::AddWaits(const Schedule& schedule, std::vector<PacketWait>& waits)
{
	if (_import == Invalid) return;
	for (Id i = 0; i < schedule.Packets.size(); ++i)
	{
		if (schedule.Packets[i].Operation == _import) _importPacket = i;
		if (schedule.Packets[i].Operation == _draw) _drawPacket = i;
	}
	if (_importPacket == Invalid || _drawPacket <= _importPacket || _drawPacket >= schedule.Packets.size())
		throw std::logic_error("Water import must precede its draw");
	if (_remote->WaitForSelected) waits.push_back({_importPacket, _remote->Selected->Ready});
}

void SceneWater::Submitted(const std::vector<CompletionToken>& completions)
{
	if (_importPacket < completions.size() && completions[_importPacket])
	{
		std::lock_guard lock(_remote->Mutex);
		_remote->Selected->Reader = completions[_importPacket];
		++_remote->Imports;
	}
}

void SceneWater::Presented(uint64_t frame, const std::vector<CompletionToken>& completions)
{
	if (!_remote || !_remote->Selected) return;
	std::lock_guard lock(_remote->Mutex);
	const auto& snapshot = *_remote->Selected;
	const auto draw = _drawPacket < completions.size() ? completions[_drawPacket] : CompletionToken{};
	if (_remote->Mode == 2)
	{
		if (!draw) return;
		uint64_t particles = 0;
		for (const auto& source : _surface.DrawSources()) particles += source.Particles->GetElementCount();
		_remote->Repeats += snapshot.Version == _remote->LastShown;
		if (snapshot.Version > _remote->LastShown + 1)
			_remote->Skipped += snapshot.Version - _remote->LastShown - 1;
		_remote->LastShown = snapshot.Version;
		++_remote->DisplayFrames;
		if (_remote->DisplayFrames <= 8 || _remote->DisplayFrames == 24)
			Log::Info("RenderGraph", "Water MGPU frame=", frame, " CrossSync=2 generation=", _remote->Generation,
			    " steps=", _remote->Ordered, " substeps=", _remote->TotalSubsteps, " simSeconds=", _remote->SimulatedSeconds,
			    " exports=", _remote->Ordered, " shown=", snapshot.Version,
			    " waitVersion=", _remote->WaitForSelected ? snapshot.Version : 0, " exportFence=", snapshot.Ready.Value,
			    " importFence=", snapshot.Reader.Value, " drawFence=", draw.Value, " drawParticles=", particles,
			    " imports=", _remote->Imports,
			    " repeats=", _remote->Repeats, " skipped=", _remote->Skipped);
		return;
	}
	if (_remote->Ordered > 3) return;
	uint64_t particles = 0;
	for (const auto& source : _surface.DrawSources()) particles += source.Particles->GetElementCount();
	Log::Info("RenderGraph", "Water MGPU frame=", frame, " CrossSync=", _remote->Mode, " generation=", _remote->Generation,
	    " ordered=", _remote->Ordered, " shown=", draw ? snapshot.Version : 0, " substeps=", _remote->Pass.Substeps(),
	    " waitVersion=", draw ? snapshot.Version : 0, " exportFence=", snapshot.Ready.Value,
	    " importFence=", snapshot.Reader.Value, " drawFence=", draw.Value,
	    " instances=", _surface.DrawSources().size(), " drawParticles=", particles,
	    " orderedExportFence=", _remote->LastOrder->Ready.Value);
}
} // namespace BistroRender

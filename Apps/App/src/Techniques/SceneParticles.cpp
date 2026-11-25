#include "SceneParticles.h"
#include "SceneGeometry.h"
#include "Common/ConsoleVariables.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12TextureResource.h"
#include "Engine.RendererDX12/GDX12MotionBlurSettings.h"
#include "Engine.RendererDX12/GDX12FrameGeneration.h"
#include "Engine.Core/Logging/Logger.h"
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <deque>

namespace BistroRender
{
namespace
{
AutoConsoleVariable<bool> AsyncSimulation(L"Render.Particles.AsyncCompute", false,
                                          L"Run particle pool initialization and simulation on the local COMPUTE queue.",
                                          EConsoleVariableFlags::Default);
AutoConsoleVariable<int> Adapter(L"Render.Particles.Adapter", -1,
    L"Secondary DXGI index for marked independent particle emitters. -1 uses GPU0.");
AutoConsoleVariable<int> CrossSync(L"Render.Particles.CrossSync", 1,
    L"Remote particles: 1 exact current, 0 exact previous (first uses current), 2 independent and 3 frame-driven (newest completed, empty until ready).",
    EConsoleVariableFlags::Default, [](int value) { return value >= 0 && value <= 3; });
AutoConsoleVariable<bool> LogSnapshots(L"Render.Particles.LogSnapshots", false,
    L"Read back visible remote particle counts for short MGPU diagnostics.");

constexpr State CopySource = D3D12_RESOURCE_STATE_COPY_SOURCE;
constexpr State CopyDest = D3D12_RESOURCE_STATE_COPY_DEST;

using ParticleClock = std::chrono::steady_clock;
using ParticleEvent = std::unique_ptr<void, decltype(&CloseHandle)>;

ParticleEvent MakeParticleEvent()
{
	ParticleEvent event(CreateEventW(nullptr, FALSE, FALSE, nullptr), CloseHandle);
	if (!event) ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
	return event;
}

auto SnapshotResources(const GDX12ParticleRenderSource& source)
{
	return std::array{source.Particles, source.Alive, source.Counters, source.Arguments};
}

void BindSourceSRV(GDX12ParticleRenderSource& source, size_t field, ID3D12Resource* buffer,
                   GDX12Descriptor& descriptor, GDX12DescriptorHeap* heap)
{
	const UINT64 width = buffer->GetDesc().Width;
	D3D12_SHADER_RESOURCE_VIEW_DESC view{};
	view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
	view.Buffer.NumElements = field == 2 ? UINT(width / sizeof(UINT)) : source.Emitter.MaxParticles;
	view.Buffer.StructureByteStride = UINT(width / view.Buffer.NumElements);
	descriptor.InitAsSRV(buffer, &view, heap, descriptor.HeapIndex == UINT_MAX
	    ? heap->GetAvailableIndex(SceneDescriptorStart) : descriptor.HeapIndex);
	source.SRVs[field] = descriptor.GPUHandle;
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
	struct InputShape
	{
		UINT64 Width = 0;
		UINT Height = 0; // Zero denotes the structured agent field.
		DXGI_FORMAT Format = DXGI_FORMAT_UNKNOWN;
		bool operator!=(const InputShape& other) const
		{
			return Width != other.Width || Height != other.Height || Format != other.Format;
		}
	};
	struct Publication
	{
		QueueSubmission::TransferBuffer Transfer;
		CompletionToken Ready, Reader;
		uint64_t Generation = 0, DisplayGeneration = 0, Version = 0, InputVersion = 0;
		std::vector<GDX12ParticleEmitter> Snapshot;
		double SimulatedSeconds = 0;
		bool Writing = false;
	};
	struct ImportedEmitter
	{
		std::array<GDX12Resource, 4> Buffers;
		std::array<UINT64, 4> Bytes{};
		std::array<GDX12Descriptor, 3> Views;
		GDX12ParticleRenderSource Bindings;
	};
	struct InputSlot
	{
		QueueSubmission::TransferBuffer Buffer;
		GDX12ParticleInteractionField Field;
		CompletionToken Ready, Reader;
		uint64_t Version = 0, Generation = 0;
		bool Pinned = false, Writing = false;
	};

	std::shared_ptr<QueueSubmission> Submit;
	Id DeviceId;
	GDX12DeviceResources Secondary;
	GDX12ParticleSimulation Solver;
	Executor GPU1;
	std::vector<ImportedEmitter> Imported;
	std::vector<Publication> Ring;
	UINT64 SnapshotBytes = 0;
	std::vector<InputSlot> InputsRing;
	GDX12Resource InputField;
	GDX12Resource InputCamera;
	GDX12Descriptor FieldSRV;
	InputShape Shape;
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT InputFootprint{};
	UINT64 CameraOffset = 0;
	UINT64 DepthCameraPayload = 0;
	uint64_t InputVersion = 0, ConsumedInput = 0;
	uint64_t LocalInputVersion = 0;
	InputSlot* InputWrite = nullptr;
	std::vector<uint64_t> Revisions;
	UINT NextUpload = 0, NextOutput = 0;
	Publication *Selected = nullptr, *LastOrder = nullptr;
	uint64_t Generation = 0, Ordered = 0, PresentedVersion = 0;
	uint64_t DisplayGeneration = 0;
	uint64_t CachedVersion = 0;
	std::vector<GDX12ParticleEmitter> PresentedSnapshot;
	struct Statistics
	{
		uint64_t Frames = 0, Exports = 0, Imports = 0, Repeats = 0, Skipped = 0, Empty = 0;
		uint64_t InputWaits = 0, OutputWaits = 0, ControlWaits = 0, InputCopies = 0;
	} Stats;
	double GameTime = 0;
	int Mode = 1;
	bool Enabled = false, DrawEnabled = false, WaitForSelected = false;

	struct ProducerInput
	{
		std::vector<GDX12ParticleEmitter> Emitters;
		GDX12ParticleSettings Settings{};
		double GameClock = 0;
		uint64_t Generation = 0;
		uint64_t DisplayGeneration = 0;
		bool Advancing = false;
	};
	std::mutex Mutex;
	std::condition_variable ControlSpace;
	std::deque<ProducerInput> Controls;
	ProducerInput LatestInput;
	double ClockValue = 0, ClockRate = 0;
	ParticleClock::time_point ClockAnchor{};
	ParticleEvent Wake{nullptr, CloseHandle}, FenceEvent{nullptr, CloseHandle};
	std::thread Worker;
	bool Stopping = false, ClockSuspended = false;
	std::exception_ptr WorkerFailure;
	ProducerInput Deferred;
	Publication* DeferredOutput = nullptr;

	Remote(GDX12DeviceResources& primary, const std::shared_ptr<QueueSubmission>& submit, Id device,
	       const std::shared_ptr<GDX12DescriptorHeap>& heap, const std::vector<GDX12ParticleEmitter>& snapshot,
	       GDX12ParticleSettings settings, uint64_t generationSeed)
	    : Submit(submit), DeviceId(device), GPU1(Submit, DeviceId, UINT(primary.FrameConstants.size()), 1), Generation(generationSeed)
	{
		Secondary.Device = &Submit->Device(DeviceId);
		Secondary.FrameConstants.resize(primary.FrameConstants.size()); // Solver needs only the upload ring size.
		Secondary.SRV_UAV_Heap = heap;
		RebuildTransport(primary, snapshot, settings);
	}

	// On reconfiguration the caller has retired GPU0 readers and stopped/drained GPU1.
	void RebuildTransport(GDX12DeviceResources& primary, const std::vector<GDX12ParticleEmitter>& snapshot,
	                      GDX12ParticleSettings settings)
	{
		Selected = LastOrder = nullptr;
		Revisions.clear(); // Force fresh publication history at the next step.
		Imported.clear();
		Ring.clear();
		SnapshotBytes = 0;
		Secondary.ParticleEmitters = snapshot;
		const bool display = settings.Enabled && settings.DrawEnabled;
		settings.Enabled = true; // Reconcile the pools even while simulation is disabled.
		Solver.BeginFrame(&Secondary, settings, false);
		const auto frame = Solver.RenderFrame();
		Imported.resize(frame.Sources.size());
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
				dest.Bytes[field] = sources[field]->D3DResource->GetDesc().Width;
				SnapshotBytes += dest.Bytes[field];
			}
		}
		if (display) PrepareDisplayTransport(primary);
		const auto gpu = Submit->Device(DeviceId).GetAdapterLuid();
		Log::Info("RenderGraph", "Particles MGPU solver GPU1 LUID=", gpu.HighPart, ":", gpu.LowPart,
		    " emitters=", Imported.size(), " snapshotBytes=", Ring.empty() ? 0 : SnapshotBytes);
		if (LogSnapshots.GetValue())
			for (const auto& source : frame.Sources)
			{
				const auto& e = source.Emitter;
				Log::Info("RenderGraph", "Particles MGPU snapshot source id=", e.Id, " blend=", unsigned(e.Blend),
				    " capacity=", e.MaxParticles, " origin=", e.World._41, ",", e.World._42, ",", e.World._43);
			}
	}

	void PrepareDisplayTransport(GDX12DeviceResources& primary)
	{
		std::lock_guard lock(Mutex);
		if (!Ring.empty() || !SnapshotBytes) return;
		auto* heap = primary.SRV_UAV_Heap.get();
		for (auto& dest : Imported)
			for (size_t field = 0; field < dest.Buffers.size(); ++field)
			{
				const auto desc = CD3DX12_RESOURCE_DESC::Buffer(dest.Bytes[field]);
				auto& resource = dest.Buffers[field];
				resource.D3DResource = primary.Device->CreateResource(D3D12_HEAP_TYPE_DEFAULT, desc, D3D12_RESOURCE_STATE_COMMON);
				if (field < dest.Views.size())
					BindSourceSRV(dest.Bindings, field, resource.D3DResource.Get(), dest.Views[field], heap);
			}
		Ring.resize(primary.FrameConstants.size() + 1);
		for (auto& slot : Ring) slot.Transfer = Submit->CreateTransferBuffer(DeviceId, SnapshotBytes);
	}

	void Drain()
	{
		StopWorker();
		GPU1.Drain();
	}

	bool External() const { return Shape.Width != 0; }
	bool DepthInput() const { return Shape.Height != 0; }

	void RebuildInput(InputShape shape)
	{
		InputsRing.clear();
		InputField.Reset();
		InputCamera.Reset();
		InputWrite = nullptr;
		Shape = shape;
		ConsumedInput = InputVersion = LocalInputVersion = 0;
		Revisions.clear();
		if (!External()) return;
		auto* device = Secondary.Device;
		const auto desc = DepthInput()
		    ? CD3DX12_RESOURCE_DESC::Tex2D(shape.Format, shape.Width, shape.Height, 1, 1)
		    : CD3DX12_RESOURCE_DESC::Buffer(shape.Width);
		InputField.D3DResource = device->CreateResource(D3D12_HEAP_TYPE_DEFAULT,
		    desc, D3D12_RESOURCE_STATE_COMMON);
		D3D12_SHADER_RESOURCE_VIEW_DESC view{};
		view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		UINT64 bytes = shape.Width;
		if (DepthInput())
		{
			view.Format = DXGI_FORMAT_R32_FLOAT;
			view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			view.Texture2D.MipLevels = 1;
			D3D12_PLACED_SUBRESOURCE_FOOTPRINT primaryFootprint{};
			UINT64 primaryBytes = 0;
			UINT rows = 0;
			UINT64 rowBytes = 0;
			device->GetDevice()->GetCopyableFootprints(&desc, 0, 1, 0, &InputFootprint, &rows, &rowBytes, &bytes);
			Submit->Device(0).GetDevice()->GetCopyableFootprints(&desc, 0, 1, 0, &primaryFootprint, nullptr, nullptr, &primaryBytes);
			if (!bytes || bytes == UINT64_MAX || bytes != primaryBytes ||
			    primaryFootprint.Footprint.RowPitch != InputFootprint.Footprint.RowPitch)
				throw std::runtime_error("Particle depth transfer requires matching adapter footprints");
			CameraOffset = (bytes + 255) & ~UINT64(255);
			bytes = CameraOffset + sizeof(GDX12CameraConstants);
			DepthCameraPayload = rows * rowBytes + sizeof(GDX12CameraConstants);
			InputCamera.D3DResource = device->CreateResource(D3D12_HEAP_TYPE_DEFAULT,
			    CD3DX12_RESOURCE_DESC::Buffer(sizeof(GDX12CameraConstants)), D3D12_RESOURCE_STATE_COMMON);
		}
		else
		{
			view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
			view.Buffer.StructureByteStride = sizeof(XMFLOAT4);
			view.Buffer.NumElements = UINT(bytes / sizeof(XMFLOAT4));
		}
		auto* heap = Secondary.SRV_UAV_Heap.get();
		const UINT descriptor = FieldSRV.DescType == DESC_TYPE_NONE ? heap->GetAvailableIndex() : FieldSRV.HeapIndex;
		FieldSRV.InitAsSRV(InputField.D3DResource.Get(), &view, heap, descriptor);
		InputsRing.resize(Secondary.FrameConstants.size() + 2);
		for (auto& slot : InputsRing) slot.Buffer = Submit->CreateTransferBuffer(DeviceId, bytes);
	}

	InputSlot* NewestInput()
	{
		InputSlot* latest = nullptr;
		for (auto& slot : InputsRing)
			if (!slot.Writing && slot.Generation == Generation && slot.Ready &&
			    (!latest || slot.Version > latest->Version) && Submit->Complete(slot.Ready))
				latest = &slot;
		return latest;
	}

	Id AppendInput(NativeWork& work, Graph& graph, std::vector<ID3D12Resource*>& bindings, GDX12ParticleInteractionField field)
	{
		return AppendInput(work, graph, bindings, field, field.Resource, nullptr, 0);
	}

	Id AppendInput(NativeWork& work, Graph& graph, std::vector<ID3D12Resource*>& bindings,
	               GDX12ParticleInteractionField field, GDX12Resource* input, GDX12Resource* camera, UINT64 cameraSourceOffset)
	{
		std::lock_guard lock(Mutex);
		const bool independent = Mode == 2;
		auto* latest = independent ? NewestInput() : nullptr;
		InputSlot* candidate = nullptr;
		InputSlot* retiring = nullptr;
		for (auto& slot : InputsRing)
		{
			if (&slot == latest || slot.Writing || slot.Pinned) continue;
			if ((slot.Ready && !Submit->Complete(slot.Ready)) ||
			    (slot.Reader && !Submit->Complete(slot.Reader)))
			{
				if (!retiring) retiring = &slot;
				continue;
			}
			candidate = &slot;
			break;
		}
		if (!candidate)
		{
			// Keep the latest complete input and let queued exports finish.
			if (independent) return Invalid;
			candidate = retiring;
			if (!candidate) throw std::logic_error("No particle input transfer slot available");
			if (candidate->Ready) Submit->Wait(candidate->Ready);
			if (candidate->Reader) Submit->Wait(candidate->Reader);
		}
		InputWrite = candidate;
		candidate->Writing = true;
		candidate->Reader = {};
		candidate->Field = field;
		auto* shared = candidate->Buffer.Resources[0].Get();
		auto* source = input->D3DResource.Get();
		std::vector<Access> copies{ResourceAccess(graph, bindings, input, Mode::Read, CopySource),
		    {NativeWork::ResourceFor(graph, bindings, shared, "shared particle input"), Mode::Write, CopyDest}};
		if (camera)
		{
			copies.push_back(ResourceAccess(graph, bindings, camera, Mode::Read));
			const Id copy = work.Add(graph, "GPU0 export particle depth and camera", std::move(copies),
			    [shared, source, camera = camera->D3DResource.Get(), cameraSourceOffset,
			     layout = InputFootprint, cameraOffset = CameraOffset](auto* cmd) {
				    const CD3DX12_TEXTURE_COPY_LOCATION dst(shared, layout), src(source, 0u);
				    cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
				    cmd->CopyBufferRegion(shared, cameraOffset, camera, cameraSourceOffset, sizeof(GDX12CameraConstants));
			    }, {}, Queue::Copy);
			graph.Operations[copy].TransferBytes = DepthCameraPayload;
			return copy;
		}
		return work.Add(graph, "GPU0 export particle interaction field", std::move(copies),
		    [shared, source, bytes = Shape.Width](auto* cmd) { cmd->CopyBufferRegion(shared, 0, source, 0, bytes); }, {}, Queue::Copy);
	}

	CompletionToken SubmitInput(CompletionToken token)
	{
		std::unique_lock lock(Mutex);
		auto* input = InputWrite;
		InputWrite = nullptr;
		input->Ready = token;
		input->Version = ++InputVersion;
		input->Generation = Generation;
		input->Writing = false;
		if (Mode == 2)
		{
			SetEvent(Wake.get());
			return {};
		}
		if (!Deferred.Generation) return {};
		const auto result = Simulate(GPU1, Deferred.Emitters, Deferred.Settings, DeferredOutput, input);
		if (result.CopiedInput) input->Reader = result.Complete;
		ConsumedInput = input->Version;
		CommitStep(DeferredOutput, result, Generation);
		Deferred.Generation = 0;
		return Selected ? Selected->Ready : CompletionToken{};
	}

	bool LatestReady() const { return Mode == 2 || Mode == 3; }

	void Configure(const std::vector<GDX12ParticleEmitter>& emitters, int mode, const GDX12ParticleSettings& settings)
	{
		std::vector<uint64_t> revisions;
		for (const auto& emitter : emitters) revisions.push_back(emitter.ResetRevision);
		if (revisions != Revisions || mode != Mode || settings.Enabled != Enabled)
		{
			Revisions = std::move(revisions);
			Mode = mode;
			Enabled = settings.Enabled;
			++Generation;
			Ordered = PresentedVersion = CachedVersion = 0;
			PresentedSnapshot.clear();
			GameTime = 0;
			LastOrder = nullptr;
			Stats = {};
			if (External())
			{
				Controls.clear();
				ControlSpace.notify_all();
			}
		}
		const bool draw = settings.Enabled && settings.DrawEnabled;
		if (DrawEnabled != draw)
		{
			DrawEnabled = draw;
			++DisplayGeneration;
			LastOrder = nullptr;
			PresentedVersion = CachedVersion = 0;
			PresentedSnapshot.clear();
		}
	}

	Publication* NewestCompleted()
	{
		Publication* latest = nullptr;
		for (auto& slot : Ring)
			if (!slot.Writing && slot.Generation == Generation && slot.DisplayGeneration == DisplayGeneration &&
			    slot.Ready && (!latest || slot.Version > latest->Version) &&
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
			if (output.Writing || &output == Selected || &output == latest) continue;
			if (latestReady && output.Ready && !Submit->Complete(output.Ready)) continue;
			if (output.Reader && !Submit->Complete(output.Reader))
			{
				if (latestReady) continue;
				++Stats.OutputWaits;
				Submit->Wait(output.Reader);
			}
			output.Ready = {};
			output.Reader = {};
			output.DisplayGeneration = DisplayGeneration;
			output.Writing = true;
			return &output;
		}
		return nullptr;
	}

	void Step(const std::vector<GDX12ParticleEmitter>& emitters, const GDX12ParticleSettings& settings, int mode)
	{
		const bool drewPreviously = DrawEnabled;
		Configure(emitters, mode, settings);
		Selected = DrawEnabled ? (LatestReady() ? NewestCompleted() : Mode == 0 ? LastOrder : nullptr) : nullptr;
		const bool bootstrap = DrawEnabled && (!LatestReady() || (!drewPreviously && Ordered));
		WaitForSelected = DrawEnabled && !LatestReady();
		auto* output = DrawEnabled ? ReserveOutput() : nullptr;
		while (bootstrap && !Selected && !output)
		{
			auto retiring = OutputRetirement();
			if (!retiring) throw std::logic_error("No slot for particle display bootstrap");
			++Stats.OutputWaits;
			Submit->Wait(retiring);
			output = ReserveOutput();
		}
		if (DrawEnabled && !LatestReady() && !output) throw std::logic_error("No slot for exact particle snapshot");
		if (External())
		{
			Deferred = {emitters, settings, 0, Generation, DisplayGeneration, false};
			DeferredOutput = output;
			if (bootstrap && output && !Selected)
			{
				output->Version = Ordered + 1;
				output->Snapshot = emitters;
				for (auto& emitter : output->Snapshot)
				{
					static_cast<GDX12ParticleEmitterSettings&>(emitter) = ResolveParticleEmitterSettings(emitter, settings.Quality);
					emitter.Paused |= !settings.SimulationEnabled;
				}
				Selected = output;
				WaitForSelected = true;
			}
		}
		else
		{
			CommitStep(output, Simulate(GPU1, emitters, settings, output), Generation);
			if (bootstrap && output && !Selected)
			{
				Selected = LastOrder;
				WaitForSelected = true;
			}
		}
	}

	struct StepResult
	{
		CompletionToken Complete;
		double Elapsed;
		bool InputWaited;
		bool CopiedInput;
		uint64_t InputVersion;
	};

	StepResult Simulate(Executor& executor, const std::vector<GDX12ParticleEmitter>& emitters,
	                    const GDX12ParticleSettings& settings, Publication* output, InputSlot* input = nullptr)
	{
		const UINT slot = NextUpload++ % UINT(Secondary.FrameConstants.size());
		const bool inputWaited = !executor.TryBegin(slot);
		if (inputWaited) executor.Begin(slot);
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
		graph.Device = DeviceId;
		graph.FrameBoundary = false;
		graph.TerminalQueue = Queue::Compute;
		std::vector<ID3D12Resource*> bindings;
		NativeWork work;
		std::vector<Access> pools;
		Solver.SetInteractionField({});
		GDX12ParticleDepthInput depth;
		// Copies and all local consumers use the same GPU1 compute queue.
		// Versions stay unique until the transport is drained and rebuilt.
		const bool copyInput = input && input->Version != LocalInputVersion;
		if (input)
		{
			if (DepthInput())
			{
				auto* camera = InputCamera.D3DResource.Get();
				depth = {&InputField, FieldSRV.GPUHandle, camera->GetGPUVirtualAddress(), UINT(Shape.Width), Shape.Height};
			}
			else
			{
				auto field = input->Field;
				field.Resource = &InputField;
				field.SRV = FieldSRV.GPUHandle;
				Solver.SetInteractionField(field);
			}
		}
		if (copyInput)
		{
			auto* shared = input->Buffer.Resources[1].Get();
			auto* target = InputField.D3DResource.Get();
			std::vector<Access> copies{
			    {NativeWork::ResourceFor(graph, bindings, shared, "shared particle input"), Mode::Read, CopySource},
			    ResourceAccess(graph, bindings, &InputField, Mode::Write, CopyDest)};
			if (DepthInput())
			{
				auto* camera = InputCamera.D3DResource.Get();
				copies.push_back(ResourceAccess(graph, bindings, &InputCamera, Mode::Write, CopyDest));
				const Id copy = work.Add(graph, "GPU1 import particle depth and camera", std::move(copies),
				    [shared, target, camera, layout = InputFootprint, cameraOffset = CameraOffset](auto* cmd) {
					    const CD3DX12_TEXTURE_COPY_LOCATION dst(target, 0u), src(shared, layout);
					    cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
					    cmd->CopyBufferRegion(camera, 0, shared, cameraOffset, sizeof(GDX12CameraConstants));
				    }, {}, Queue::Compute);
				graph.Operations[copy].TransferBytes = DepthCameraPayload;
			}
			else
				work.Add(graph, "GPU1 import particle interaction field", std::move(copies),
				    [shared, target, bytes = Shape.Width](auto* cmd) { cmd->CopyBufferRegion(target, 0, shared, 0, bytes); },
				    {}, Queue::Compute);
		}
		for (auto* buffer : Solver.GetPoolResources())
			pools.push_back(ResourceAccess(graph, bindings, buffer, Mode::ReadWrite, UnorderedAccess));
		work.AddOwned(graph, "GPU1 initialize particle pools", pools, bindings, [this](auto* cmd) {
			Solver.InitializePools(cmd, &Secondary);
		}, "Particle initialization must not submit", {}, Queue::Compute);
		AddResources(graph, bindings, pools, Solver.GetConstantResources(slot), Mode::Read);
		if (input) pools.push_back(ResourceAccess(graph, bindings, &InputField, Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
		if (depth.Resource)
			pools.push_back(ResourceAccess(graph, bindings, &InputCamera, Mode::Read, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER));
		work.AddOwned(graph, "GPU1 simulate particles", std::move(pools), bindings, [this, depth](auto* cmd) {
			Solver.ExecuteWithDepth(cmd, &Secondary, depth);
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
		std::vector<PacketWait> waits;
		if (copyInput) waits.push_back({0, input->Ready});
		executor.Execute(schedule, bindings, [&](Id op, uint32_t, auto* cmd) { work.Record(op, cmd); }, nullptr, nullptr, nullptr, waits);
		if (copyInput) LocalInputVersion = input->Version;
		return {executor.Completions().back(), elapsed, inputWaited, copyInput, input ? input->Version : 0};
	}

	void CommitStep(Publication* output, StepResult result, uint64_t generation)
	{
		if (output)
		{
			output->Writing = false;
			output->Ready = result.Complete;
			output->Generation = generation;
			output->InputVersion = result.InputVersion;
		}
		if (generation != Generation) return;
		++Ordered;
		GameTime += result.Elapsed;
		Stats.InputWaits += result.InputWaited;
		Stats.InputCopies += result.CopiedInput;
		if (output && output->DisplayGeneration == DisplayGeneration)
		{
			output->Version = Ordered;
			output->SimulatedSeconds = GameTime;
			LastOrder = output;
			++Stats.Exports;
			ControlSpace.notify_all();
		}
	}

	double GameClockAt(ParticleClock::time_point now) const
	{
		return ClockValue + ClockRate * std::chrono::duration<double>(now - ClockAnchor).count();
	}

	static bool Paused(const ProducerInput& input, size_t i)
	{
		return !input.Advancing || input.Emitters[i].Paused || input.Emitters[i].TimeScale <= 0;
	}

	static bool NeedsControl(const ProducerInput& prior, const ProducerInput& next)
	{
		// The latest snapshot carries the cumulative burst, including while disabled.
		if (prior.Generation != next.Generation || prior.DisplayGeneration != next.DisplayGeneration || prior.Advancing != next.Advancing ||
		    prior.Settings.Enabled != next.Settings.Enabled) return true;
		for (size_t i = 0; i < next.Emitters.size(); ++i)
			if (Paused(prior, i) != Paused(next, i))
				return true;
		return false;
	}

	CompletionToken OutputRetirement()
	{
		auto* latest = NewestCompleted();
		for (const auto& slot : Ring)
		{
			if (&slot == Selected || &slot == latest) continue;
			if (slot.Ready && !Submit->Complete(slot.Ready)) return slot.Ready;
			if (slot.Reader && !Submit->Complete(slot.Reader)) return slot.Reader;
		}
		return {};
	}

	void WaitForProducerWork(CompletionToken token, CompletionToken& armed)
	{
		if (token && (token.Device != armed.Device || token.Target != armed.Target ||
		              token.Value != armed.Value || token.Incarnation != armed.Incarnation))
		{
			Submit->NotifyOnCompletion(token, FenceEvent.get());
			armed = token;
		}
		const HANDLE events[] = {Wake.get(), FenceEvent.get()};
		const auto result = WaitForMultipleObjects(2, events, FALSE, token ? 100 : INFINITE);
		if (result == WAIT_FAILED) ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
		if (result == WAIT_TIMEOUT) Submit->CheckHealth();
	}

	void Produce()
	{
		Executor executor(Submit, DeviceId, UINT(Secondary.FrameConstants.size()), 1);
		ProducerInput prior;
		CompletionToken pending, armed;
		bool bootstrap = false;
		try
		{
			std::unique_lock lock(Mutex);
			while (!Stopping)
			{
				const auto sleep = [&](CompletionToken completion) {
					lock.unlock();
					WaitForProducerWork(completion, armed);
					lock.lock();
				};
				if (pending && !Submit->Complete(pending))
				{
					sleep(pending);
					continue;
				}
				if (ClockSuspended)
				{
					sleep({});
					continue;
				}
				const bool control = !Controls.empty();
				const bool steady = LatestInput.Advancing && std::any_of(LatestInput.Emitters.begin(),
				    LatestInput.Emitters.end(), [](const auto& e) { return !e.Paused && e.TimeScale > 0; });
				if (!control && !steady && !bootstrap)
				{
					sleep({});
					continue;
				}
				auto* external = External() ? NewestInput() : nullptr;
				if (control && (!Controls.front().Settings.Enabled || (External() && !external)))
				{
					// A control can reach the worker before any compatible GPU input.
					// Keep its clock boundary. LatestInput retains the cumulative burst.
					prior = std::move(Controls.front());
					Controls.pop_front();
					bootstrap = prior.Settings.Enabled;
					ControlSpace.notify_one();
					continue;
				}
				if (External() && !external)
				{
					CompletionToken incoming;
					for (const auto& slot : InputsRing)
						if (!slot.Writing && slot.Generation == Generation && slot.Ready)
							incoming = slot.Ready;
					sleep(incoming);
					continue;
				}
				const auto& next = control ? Controls.front() : LatestInput;
				const bool display = DrawEnabled && next.DisplayGeneration == DisplayGeneration;
				auto* output = display ? ReserveOutput() : nullptr;
				if (display && !output)
				{
					sleep(OutputRetirement());
					continue;
				}
				ProducerInput input = control ? std::move(Controls.front()) : LatestInput;
				if (control)
				{
					Controls.pop_front();
					ControlSpace.notify_one();
				}
				else input.GameClock = GameClockAt(ParticleClock::now());
				const auto elapsed = (std::max)(0.0, input.GameClock - prior.GameClock);
				auto stepEmitters = input.Emitters;
				for (size_t i = 0; i < stepEmitters.size(); ++i)
				{
					auto& emitter = stepEmitters[i];
					emitter.Paused = Paused(input, i);
					emitter.DeltaTime = !bootstrap && input.Generation == prior.Generation && !emitter.Paused && !Paused(prior, i)
					    ? float(elapsed) : 0.f;
				}
				auto settings = input.Settings;
				settings.Enabled = true; // Apply paused controls and resets without drawing.
				if (external) external->Pinned = true;
				lock.unlock();
				const auto step = Simulate(executor, stepEmitters, settings, output, external);
				lock.lock();
				if (external)
				{
					if (step.CopiedInput) external->Reader = step.Complete;
					external->Pinned = false;
					ConsumedInput = external->Version;
				}
				CommitStep(output, step, input.Generation);
				pending = step.Complete;
				prior = std::move(input);
				bootstrap = false;
			}
		}
		catch (...)
		{
			std::lock_guard lock(Mutex);
			WorkerFailure = std::current_exception();
			ControlSpace.notify_all();
		}
		executor.Drain();
	}

	void StopWorker()
	{
		if (!Worker.joinable()) return;
		{
			std::lock_guard lock(Mutex);
			Stopping = true;
		}
		SetEvent(Wake.get());
		Worker.join();
		if (WorkerFailure) std::rethrow_exception(WorkerFailure);
		Controls.clear();
		LatestInput = {};
	}

	void PauseClock()
	{
		if (!Worker.joinable()) return;
		std::lock_guard lock(Mutex);
		const auto now = ParticleClock::now();
		ClockValue = GameClockAt(now);
		ClockAnchor = now;
		ClockRate = 0;
		LatestInput.Advancing = false;
		ClockSuspended = true;
		SetEvent(Wake.get());
	}

	void Prepare(const std::vector<GDX12ParticleEmitter>& emitters, const GDX12ParticleSettings& settings,
	             int mode, float worldRate)
	{
		if (mode != 2)
		{
			StopWorker();
			Selected = nullptr;
			WaitForSelected = false;
			if (settings.Enabled) Step(emitters, settings, mode);
			return;
		}
		if (!Worker.joinable())
		{
			GPU1.Drain();
			Wake = MakeParticleEvent();
			FenceEvent = MakeParticleEvent();
			ClockValue = ClockRate = 0;
			ClockAnchor = ParticleClock::now();
			Stopping = ClockSuspended = false;
			Worker = std::thread([this] {
				try { Produce(); }
				catch (...)
				{
					std::lock_guard lock(Mutex);
					WorkerFailure = std::current_exception();
					ControlSpace.notify_all();
				}
			});
		}
		std::unique_lock lock(Mutex);
		if (WorkerFailure) std::rethrow_exception(WorkerFailure);
		const bool drewPreviously = DrawEnabled;
		Configure(emitters, mode, settings);
		const bool active = settings.Enabled && settings.SimulationEnabled && worldRate > 0;
		ProducerInput next{emitters, settings, 0, Generation, DisplayGeneration, active};
		// The previous frame has already registered its copy fence. Drop its pin
		// before any input back pressure, including with a two-slot output ring.
		Selected = nullptr;
		ClockSuspended = false;
		if (!settings.Enabled && LatestInput.Settings.Enabled) Controls.clear();
		if (NeedsControl(LatestInput, next) && Controls.size() == 8)
		{
			++Stats.ControlWaits;
			SetEvent(Wake.get());
			ControlSpace.wait(lock, [this] { return Controls.size() < 8 || WorkerFailure; });
			if (WorkerFailure) std::rethrow_exception(WorkerFailure);
		}
		const auto now = ParticleClock::now();
		ClockValue = next.GameClock = GameClockAt(now);
		ClockAnchor = now;
		ClockRate = active ? worldRate : 0;
		if (NeedsControl(LatestInput, next)) Controls.push_back(next);
		LatestInput = std::move(next);
		Selected = DrawEnabled ? NewestCompleted() : nullptr;
		WaitForSelected = false;
		SetEvent(Wake.get());
		// The first visible frame may need the producer to publish a fresh snapshot.
		// An external input from this frame is not available until GPU0 submits it.
		const bool hasInput = !External() || std::any_of(InputsRing.begin(), InputsRing.end(), [&](const auto& slot) {
			return !slot.Writing && slot.Generation == Generation && bool(slot.Ready);
		});
		if (DrawEnabled && !drewPreviously && Ordered && !Selected && hasInput)
		{
			ControlSpace.wait(lock, [this] { return LastOrder || WorkerFailure; });
			if (WorkerFailure) std::rethrow_exception(WorkerFailure);
			Selected = NewestCompleted();
			if (!Selected)
			{
				Selected = LastOrder;
				WaitForSelected = true;
			}
		}
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
	for (auto& lane : _remote)
		if (lane.Solver)
		{
			try { lane.Solver->Drain(); }
			catch (...) { (void)lane.Solver.release(); } // A quarantined queue can still reference the buffers.
		}
	_pass.ClearDenendencies();
	if (_resources.ParticleSimulation)
	{
		_resources.ParticleSimulation->SetInteractionField({});
	}
}
void SceneParticles::Packet(Graph& graph, const std::vector<ID3D12Resource*>& bindings, const char* name,
                            std::vector<Access> accesses, std::function<void(GDX12CommandList*)> record,
                            Queue target, NativeWork* work)
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
	(work ? *work : _work).AddOwned(graph, name, std::move(unique), bindings, std::move(record),
	               "Particle packets cannot submit private signals", {}, target);
}
int SceneParticles::SecondaryAdapter() { return Adapter.GetValue(); }

void SceneParticles::PauseIndependentClock()
{
	for (auto& lane : _remote)
		if (lane.Solver) lane.Solver->PauseClock();
}

void SceneParticles::PreserveConsumedBursts(const GDX12ParticleSimulation& simulation)
{
	for (const auto& consumed : simulation.ConsumedBursts())
	{
		auto& saved = _consumedBursts[consumed.Id];
		if (!saved.Total || consumed.ResetRevision > saved.ResetRevision ||
		    (saved.ResetRevision == consumed.ResetRevision && consumed.Total > saved.Total))
			saved = consumed;
	}
}

void SceneParticles::BeginSimulation(GDX12ParticleInteractionField interaction, GDX12Texture* depth,
                                     Executor& primary, Id device, float worldRate, bool ownerDraw)
{
	_work.Commands.clear();
	for (auto& lane : _remote)
	{
		lane.Export = lane.Import = lane.ImportPacket = Invalid;
		lane.Consumed = false;
	}
	for (auto& sample : _visibilitySamples.at(_resources.CurrFrameConstantsIndex))
	{
		sample.Written = false;
		if (!sample.Frame) continue;
		const D3D12_RANGE bytes{0, (sample.Sources.size() + 1) * sizeof(UINT)};
		void* mapped = nullptr;
		ThrowIfFailed(sample.Buffer.D3DResource->Map(0, &bytes, &mapped));
		const auto* counts = static_cast<const UINT*>(mapped);
		for (size_t i = 0; i < sample.Sources.size(); ++i)
		{
			const auto [id, version] = sample.Sources[i];
			if (!version) continue;
			const UINT visible = counts[i + 1] - counts[i];
			if (std::find(_visibleSources.begin(), _visibleSources.end(), id) != _visibleSources.end()) continue;
			if (sample.Frame <= 12 || visible)
				Log::Info("RenderGraph", "Particles MGPU visible owner=", primary.Submissions()->AdapterLabel(sample.Owner),
				    " frame=", sample.Frame, " shown=", version,
				    " id=", id, " remoteDrawElements=", visible);
			if (visible) _visibleSources.push_back(id);
		}
		const D3D12_RANGE noWrites{0, 0};
		sample.Buffer.D3DResource->Unmap(0, &noWrites);
		sample.Frame = 0;
	}
	for (auto& emitter : _resources.ParticleEmitters)
	{
		if (emitter.Blend == ParticleBlend::Opaque) continue;
		const auto saved = _consumedBursts.find(emitter.Id);
		if (saved != _consumedBursts.end() && saved->second.ResetRevision == emitter.ResetRevision)
			emitter.BurstBaseline = (std::max)(emitter.BurstBaseline, saved->second.Total);
	}
	_interaction = interaction;
	auto* simulation = _resources.ParticleSimulation.get();
	simulation->SetInteractionField(interaction);
	const auto settings = GetParticleSettings();
	const auto& submit = primary.Submissions();
	const bool requested = device != 0;
	std::array<std::vector<GDX12ParticleEmitter>, 3> remoteEmitters;
	std::array<std::vector<EmitterLayout>, 3> layouts;
	if (requested)
		for (const auto& emitter : _resources.ParticleEmitters)
		{
			if ((ownerDraw && emitter.Blend != ParticleBlend::Opaque) || !emitter.SimulateOnSecondary || emitter.InteractionEnabled != (emitter.Blend == ParticleBlend::Opaque) ||
			    (emitter.DepthCollision && (emitter.InteractionEnabled || !depth))) continue;
			const auto group = emitter.DepthCollision ? Inputs::Depth : emitter.InteractionEnabled ? Inputs::Interaction : Inputs::Frame;
			remoteEmitters[size_t(group)].push_back(emitter);
		}
	const UINT64 fieldBytes = interaction.Resource ? interaction.Resource->D3DResource->GetDesc().Width : 0;
	const auto depthDesc = depth ? depth->GetResource()->D3DResource->GetDesc() : D3D12_RESOURCE_DESC{};
	const std::array<Remote::InputShape, 3> inputs{{{}, {fieldBytes},
	    {depthDesc.Width, depthDesc.Height, depthDesc.Format}}};
	std::array<bool, 3> rebuild{}, replaceInput{};
	for (size_t i = 0; i < _remote.size(); ++i)
	{
		auto& emitters = remoteEmitters[i];
		std::sort(emitters.begin(), emitters.end(), [](const auto& a, const auto& b) { return a.Id < b.Id; });
		auto& layout = layouts[i];
		for (const auto& emitter : emitters)
			layout.push_back({emitter.Id, ResolveParticleEmitterSettings(emitter, settings.Quality).MaxParticles});
		auto& lane = _remote[i];
		const bool deviceChanged = device != lane.Device;
		rebuild[i] = layout != lane.Layout;
		replaceInput[i] = lane.Solver && lane.Solver->Shape != inputs[i];
		if (!(deviceChanged || rebuild[i] || replaceInput[i])) continue;
		if (lane.Solver)
		{
			primary.Drain();
			lane.Solver->Drain();
			lane.Generation = lane.Solver->Generation;
			if (ownerDraw) PreserveConsumedBursts(lane.Solver->Solver);
			for (auto& emitter : _resources.ParticleEmitters)
				emitter.BurstBaseline = (std::max)(emitter.BurstBaseline,
				    lane.Solver->Solver.ConsumedBurst(emitter.Id, emitter.ResetRevision));
			if (deviceChanged || layout.empty()) lane.Solver.reset();
		}
		lane.Device = device;
		lane.Layout = layout;
		lane.Attempted = false;
	}
	if (_heapDevice != device)
	{
		_secondaryHeap.reset();
		_heapDevice = device;
	}
	std::vector<uint64_t> remoteIds;
	for (size_t i = 0; i < _remote.size(); ++i)
	{
		auto& lane = _remote[i];
		auto& emitters = remoteEmitters[i];
		const bool create = !lane.Solver && requested && !emitters.empty() && settings.Enabled && !lane.Attempted;
		const bool display = lane.Solver && settings.Enabled && settings.DrawEnabled && lane.Solver->Ring.empty();
		if (create || (lane.Solver && (rebuild[i] || replaceInput[i] || display)))
		{
			lane.Attempted = true;
			const auto fallback = [&] {
				if (display && !rebuild[i] && !replaceInput[i])
				{
					primary.Drain();
					lane.Solver->Drain();
					lane.Generation = lane.Solver->Generation;
					for (auto& emitter : _resources.ParticleEmitters)
						emitter.BurstBaseline = (std::max)(emitter.BurstBaseline,
						    lane.Solver->Solver.ConsumedBurst(emitter.Id, emitter.ResetRevision));
				}
				lane.Solver.reset();
			};
			for (auto& emitter : emitters)
			{
				const auto& live = *std::find_if(_resources.ParticleEmitters.begin(), _resources.ParticleEmitters.end(),
				    [&](const auto& source) { return source.Id == emitter.Id; });
				emitter.BurstBaseline = (std::max)(live.BurstBaseline, simulation->ConsumedBurst(emitter.Id, emitter.ResetRevision));
			}
			try
			{
				if (create)
				{
					// Pool views stay fixed when another group changes. All allocations
					// happen here on the frame owner, before a producer starts.
					if (!_secondaryHeap)
						_secondaryHeap = std::make_shared<GDX12DescriptorHeap>(&submit->Device(device),
						    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, _resources.SRV_UAV_Heap->GetNumDescriptors(),
						    D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
					lane.Solver = std::make_unique<Remote>(_resources, submit, device, _secondaryHeap, emitters, settings, lane.Generation);
				}
				else if (rebuild[i])
					lane.Solver->RebuildTransport(_resources, emitters, settings);
				if (create || replaceInput[i]) lane.Solver->RebuildInput(inputs[i]);
				if (display) lane.Solver->PrepareDisplayTransport(_resources);
			}
			catch (const DxException& e)
			{
				fallback();
				Log::Warning("RenderGraph", "Local particles fallback: ", e.ToString());
			}
			catch (const std::exception& e)
			{
				fallback();
				Log::Warning("RenderGraph", "Local particles fallback: ", e.what());
			}
		}
		if (!lane.Solver) continue;
		for (const auto& item : lane.Layout) remoteIds.push_back(item.Id);
		lane.Solver->Prepare(emitters, settings, CrossSync.GetValue(), worldRate);
	}
	if (ownerDraw)
		for (const auto& emitter : _resources.ParticleEmitters)
			if (emitter.Blend != ParticleBlend::Opaque) remoteIds.push_back(emitter.Id);
	simulation->BeginFrame(&_resources, _common, &remoteIds);
	_renderFrame = simulation->RenderFrame();
	_renderFrame.ResetMotion |= _common.ResetHistory;
	_firstRemote = UINT(_renderFrame.Sources.size());
	_remoteVersions.clear();
	for (const auto& lane : _remote)
		if (lane.Solver && lane.Solver->Selected)
		{
			const auto* selected = lane.Solver->Selected;
			lane.Solver->AppendSources(_renderFrame);
			_remoteVersions.insert(_remoteVersions.end(), selected->Snapshot.size(), selected->Version);
		}
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

void SceneParticles::AddExchanges(const Schedule& schedule, std::vector<PacketWait>& waits,
                                  std::vector<Executor::PacketExchange>& exchanges)
{
	for (auto& lane : _remote)
	{
		if (!lane.Solver) continue;
		auto* remote = lane.Solver.get();
		lane.ImportPacket = schedule.OptionalPacketFor(lane.Import);
		const auto producerPacket = schedule.OptionalPacketFor(lane.Export);
		const auto waitPacket = remote->WaitForSelected ? lane.ImportPacket : Invalid;
		const bool independent = remote->Mode == 2;
		if (producerPacket != Invalid)
			exchanges.push_back({producerPacket, independent ? Invalid : waitPacket,
			    [remote](CompletionToken token) { return remote->SubmitInput(token); }});
		if (waitPacket != Invalid && (producerPacket == Invalid || independent))
			waits.push_back({waitPacket, remote->Selected->Ready});
	}
}

Id SceneParticles::ExecutionDevice() const
{
	if (GetParticleSettings().Enabled)
		for (const auto& lane : _remote)
			if (lane.Solver && !lane.Layout.empty()) return lane.Solver->DeviceId;
	return 0;
}

bool SceneParticles::BenchmarkReady() const
{
	const auto settings = GetParticleSettings();
	if (!settings.Enabled || !settings.DrawEnabled) return true;
	for (const auto& lane : _remote)
		if (lane.Solver && !lane.Layout.empty() && !lane.Solver->Selected) return false;
	return true;
}

void SceneParticles::Submitted(const std::vector<CompletionToken>& completions)
{
	for (auto& lane : _remote)
		if (lane.ImportPacket < completions.size() && completions[lane.ImportPacket])
		{
			auto& remote = *lane.Solver;
			std::lock_guard lock(remote.Mutex);
			remote.Selected->Reader = completions[lane.ImportPacket];
			remote.CachedVersion = remote.Selected->Version;
			if (++remote.Stats.Imports == 1)
				remote.Submit->LogFirstImport("Particles", remote.Selected->Ready, remote.Selected->Reader);
		}
}

void SceneParticles::Presented(uint64_t frame, std::vector<SimulationPublication>* publications)
{
	for (auto& sample : _visibilitySamples.at(_resources.CurrFrameConstantsIndex))
		if (sample.Written) sample.Frame = frame;
	for (size_t i = 0; i < _remote.size(); ++i)
	{
		auto& lane = _remote[i];
		if (!lane.Solver) continue;
		auto& remote = *lane.Solver;
		std::lock_guard lock(remote.Mutex);
		const auto* selected = remote.Selected;
		if (publications && lane.Consumed && selected)
			publications->push_back({"ParticleAdapter", remote.DeviceId, i, selected->Generation,
			    selected->Version, selected->SimulatedSeconds});
		const auto previous = remote.PresentedVersion;
		remote.PresentedVersion = selected ? selected->Version : 0;
		if (selected) remote.PresentedSnapshot = selected->Snapshot;
		auto& stats = remote.Stats;
		++stats.Frames;
		if (!selected) ++stats.Empty;
		else if (remote.PresentedVersion == previous) ++stats.Repeats;
		else if (remote.PresentedVersion > previous + 1) stats.Skipped += remote.PresentedVersion - previous - 1;
		if (stats.Frames <= 12 && remote.Ordered)
			Log::Info("RenderGraph", "Particles MGPU frame=", frame, " CrossSync=", remote.Mode,
			    " generation=", remote.Generation, " ordered=", remote.Ordered, " exported=", remote.LastOrder ? remote.LastOrder->Version : 0,
			    " shown=", remote.PresentedVersion, " exportFence=", selected ? selected->Ready.Value : 0,
			    " importFence=", lane.ImportPacket != Invalid && selected ? selected->Reader.Value : 0,
			    " group=", i, " inputVersion=", remote.ConsumedInput, " shownInputVersion=", selected ? selected->InputVersion : 0,
			    " readyAtChoice=", remote.LatestReady() && selected, " gameSeconds=", remote.GameTime);
		if (LogSnapshots.GetValue() && stats.Frames == 24)
			Log::Info("RenderGraph", "Particles MGPU summary frame=", frame, " CrossSync=", remote.Mode,
			    " generation=", remote.Generation, " frames=", stats.Frames, " steps=", remote.Ordered,
			    " publications=", stats.Exports, " imports=", stats.Imports, " repeats=", stats.Repeats,
			    " skippedShows=", stats.Skipped, " noSnapshotFrames=", stats.Empty,
			    " inputRetirementWaits=", stats.InputWaits, " outputRetirementWaits=", stats.OutputWaits,
			    " controlBackPressure=", stats.ControlWaits, " inputCopies=", stats.InputCopies, " gameSeconds=", remote.GameTime,
			    " shownGameSeconds=", selected ? selected->SimulatedSeconds : 0.0);
	}
}
void SceneParticles::AppendSimulation(Graph& graph, std::vector<ID3D12Resource*>& bindings, Inputs inputs,
                                      GDX12Texture* depth)
{
	auto& lane = _remote[size_t(inputs)];
	if (lane.Solver && lane.Solver->External() && GetParticleSettings().Enabled)
	{
		if (inputs == Inputs::Depth)
		{
			auto& camera = *_resources.FrameConstants.at(_resources.CurrFrameConstantsIndex)->CameraCB;
			auto& resource = camera.GetResource();
			const auto offset = camera.GetElementAddress(_common.ActiveCameraCBufferIndex) - resource.D3DResource->GetGPUVirtualAddress();
			lane.Export = lane.Solver->AppendInput(_work, graph, bindings, {}, depth->GetResource(), &resource, offset);
		}
		else
			lane.Export = lane.Solver->AppendInput(_work, graph, bindings, _interaction);
	}
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
	AppendSimulationImports(graph, bindings, opaque);
	auto* readback = VisibilityReadback(slot, opaque, *_resources.Device, graph.Device);
	if (readback) preparation.push_back(ResourceAccess(graph, bindings, readback, Mode::Write, CopyDest));
	Packet(graph, bindings, opaque ? "opaque particles frustum cull" : "transparent particles cull and global sort",
	       std::move(preparation), [this, opaque, readback](auto* cmd) {
		       _pass.PrepareRenderData(cmd, _renderFrame, opaque, readback, _firstRemote);
	       });
}
void SceneParticles::AppendSimulationImports(Graph& graph, std::vector<ID3D12Resource*>& bindings, bool opaque)
{
	for (size_t i = 0; i < _remote.size(); ++i)
	{
		auto& lane = _remote[i];
		const bool interaction = i == size_t(Inputs::Interaction);
		if (lane.Solver && interaction == opaque)
		{
			lane.Import = lane.Solver->AppendImport(_work, graph, bindings);
			lane.Consumed = lane.Solver->Selected && std::any_of(lane.Solver->Selected->Snapshot.begin(),
			    lane.Solver->Selected->Snapshot.end(), [opaque](const auto& emitter) {
				    return (emitter.Blend == ParticleBlend::Opaque) == opaque;
			    });
		}
	}
}
GDX12Resource* SceneParticles::VisibilityReadback(UINT slot, bool opaque, GDX12Device& device, Id owner)
{
	auto& sample = _visibilitySamples.at(slot)[opaque];
	if (LogSnapshots.GetValue() && std::any_of(_renderFrame.Sources.begin() + _firstRemote, _renderFrame.Sources.end(),
	    [&](const auto& source) {
		    return (source.Emitter.Blend == ParticleBlend::Opaque) == opaque &&
		        std::find(_visibleSources.begin(), _visibleSources.end(), source.Emitter.Id) == _visibleSources.end();
	    }))
	{
		auto& buffer = sample.Buffer;
		sample.Sources.clear();
		for (size_t i = _firstRemote; i < _renderFrame.Sources.size(); ++i)
		{
			const auto& emitter = _renderFrame.Sources[i].Emitter;
			sample.Sources.emplace_back(emitter.Id, (emitter.Blend == ParticleBlend::Opaque) == opaque
			    ? _remoteVersions[i - _firstRemote] : 0);
		}
		const UINT64 bytes = (sample.Sources.size() + 1) * sizeof(UINT);
		if (sample.Owner != owner || !buffer.D3DResource || buffer.D3DResource->GetDesc().Width < bytes)
			buffer.D3DResource = device.CreateResource(D3D12_HEAP_TYPE_READBACK,
			    CD3DX12_RESOURCE_DESC::Buffer(bytes), D3D12_RESOURCE_STATE_COPY_DEST);
		sample.Owner = owner;
		sample.Written = true;
		return &buffer;
	}
	return nullptr;
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

void SceneParticles::AppendRemoteWeighted(Graph& graph, std::vector<ID3D12Resource*>& bindings,
    NativeWork& work, GDX12DeviceResources& owner, GDX12DeviceResources& simulationResources,
    GDX12ParticleSimulation& simulation, GDX12Texture* accum, GDX12Texture* reveal, GDX12Texture* depth,
    Id cameraId, D3D12_GPU_VIRTUAL_ADDRESS camera, UINT slot, RemoteRaster& raster,
    const std::function<ID3D12Resource*(GDX12Texture&)>& asset)
{
	raster.Reads.clear();
	simulationResources.CurrFrameConstantsIndex = slot;
	simulationResources.ParticleEmitters.clear();
	for (const auto& emitter : _resources.ParticleEmitters)
		if (emitter.Blend != ParticleBlend::Opaque)
			simulationResources.ParticleEmitters.push_back(emitter);
	const auto settings = GetParticleSettings();
	simulation.BeginFrame(&simulationResources, settings, _common.ResetHistory);
	raster.Frame = simulation.RenderFrame();
	if (raster.Frame.Sources.empty()) return;

	auto* depthResource = depth ? depth->GetResource() : nullptr;
	const State cameraState = cameraId != Invalid && graph.Resources.at(cameraId).Memory == Heap::Upload ?
	    D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
	const Access cameraAccess{cameraId, Mode::Read, cameraState};
	std::vector<Access> pools;
	AddResources(graph, bindings, pools, simulation.GetPoolResources(), Mode::ReadWrite);
	Packet(graph, bindings, "owner initialize transparent particle pools", pools,
	    [&simulation, &simulationResources](auto* cmd) { simulation.InitializePools(cmd, &simulationResources); },
	    Queue::Direct, &work);
	AddResources(graph, bindings, pools, simulation.GetConstantResources(slot), Mode::Read);
	GDX12ParticleDepthInput input{};
	if (std::any_of(raster.Frame.Sources.begin(), raster.Frame.Sources.end(),
	        [](const auto& source) { return source.Emitter.DepthCollision; }))
	{
		if (!depthResource || cameraId == Invalid) throw std::logic_error("Owner particle collision requires prepass depth and camera");
		auto* simHeap = simulationResources.SRV_UAV_Heap.get();
		auto depthView = depth->ShaderViewDescription();
		auto& descriptor = raster.SimulationDepth;
		descriptor.InitAsSRV(depthResource->D3DResource.Get(), &depthView, simHeap,
		    descriptor.HeapIndex == UINT_MAX ? simHeap->GetAvailableIndex() : descriptor.HeapIndex);
		input = {depthResource, descriptor.GPUHandle, camera, depth->GetWidth(), depth->GetHeight()};
		pools.push_back(ResourceAccess(graph, bindings, depthResource, Mode::Read,
		    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
		pools.push_back(cameraAccess);
	}
	Packet(graph, bindings, "owner simulate transparent particles", std::move(pools),
	    [&simulation, &simulationResources, input](auto* cmd) { simulation.ExecuteWithDepth(cmd, &simulationResources, input); },
	    Queue::Direct, &work);
	if (!settings.DrawEnabled) return;
	if (!depthResource || !accum || !reveal || cameraId == Invalid)
		throw std::logic_error("Owner particle raster requires WBOIT targets and opaque depth");

	raster.Pass.Setup(&owner, &raster.Common);
	raster.Pass.SetReadOnlyDepthEnabled(false);
	raster.Pass.SetMotionOutputEnabled(false);
	raster.Reads = {cameraAccess, ResourceAccess(graph, bindings, depthResource, Mode::Read)};
	auto* heap = owner.SRV_UAV_Heap.get();
	raster.SourceViews.resize(raster.Frame.Sources.size());
	for (size_t i = 0; i < raster.Frame.Sources.size(); ++i)
	{
		auto& source = raster.Frame.Sources[i];
		auto& entry = raster.SourceViews[i];
		if (!entry) entry = std::make_unique<std::array<GDX12Descriptor, 3>>();
		auto& views = *entry;
		const auto buffers = SnapshotResources(source);
		for (size_t field = 0; field < views.size(); ++field)
			BindSourceSRV(source, field, buffers[field]->D3DResource.Get(), views[field], heap);
		if (source.Emitter.Texture)
		{
			auto* atlas = source.Emitter.Texture->PrimaryDeviceTexture;
			if (!atlas) throw std::logic_error("Particle atlas has no primary texture");
			const Id id = NativeWork::ResourceFor(graph, bindings, asset(*atlas), "WBOIT particle atlas");
			raster.Reads.push_back({id, Mode::Read, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE});
		}
	}
	raster.Pass.SetInputs({depth, accum, reveal});
	raster.Pass.Initialize();
	raster.Pass.BeginFrame(depth, raster.Frame, camera, true);
	std::vector<Access> preparation{cameraAccess};
	AddAccesses(graph, bindings, preparation, raster.Pass.NativePreparationAccesses(raster.Frame, false));
	Packet(graph, bindings, "owner transparent particle cull and global sort", std::move(preparation),
	    [&raster](auto* cmd) { raster.Pass.PrepareRenderData(cmd, raster.Frame, false); }, Queue::Direct, &work);
	auto weighted = raster.Reads;
	AddAccesses(graph, bindings, weighted, raster.Pass.NativeRasterAccesses(true));
	for (auto* target : {accum, reveal})
		weighted.push_back(ResourceAccess(graph, bindings, target->GetResource(), Mode::ReadWrite,
		    D3D12_RESOURCE_STATE_RENDER_TARGET));
	Packet(graph, bindings, "owner weighted particles", std::move(weighted),
	    [&raster](auto* cmd) { raster.Pass.DrawWeightedAndMotion(cmd); }, Queue::Direct, &work);
}

void SceneParticles::AppendRemoteComposite(Graph& remote, std::vector<ID3D12Resource*>& bindings,
    NativeWork& work, GDX12Texture* hdr, RemoteRaster& raster)
{
	if (raster.Reads.empty()) return;
	auto accesses = raster.Reads;
	AddAccesses(remote, bindings, accesses, raster.Pass.NativeRasterAccesses(false));
	accesses.push_back(ResourceAccess(remote, bindings, hdr->GetResource(), Mode::ReadWrite,
	                                  D3D12_RESOURCE_STATE_RENDER_TARGET));
	Packet(remote, bindings, "owner SortedAlpha and additive HDR particles", std::move(accesses),
	       [&raster, hdr](auto* cmd) { raster.Pass.Composite(cmd, hdr); }, Queue::Direct, &work);
}
} // namespace BistroRender

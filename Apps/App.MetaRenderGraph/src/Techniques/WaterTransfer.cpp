#include "WaterTransfer.h"
#include "NativeWork.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include "Engine.Core/Logging/Logger.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <set>
#include <thread>

namespace Meta {
namespace {
constexpr UINT Slots = 3;
static_assert(sizeof(XMFLOAT3) == 12, "water transport contains only tightly packed positions");
template<class T> auto Buffer(GDX12Device& device, GDX12DescriptorHeap& heap, UINT count) {
    auto b = std::make_unique<GDX12UploadBuffer<T>>(&device, count, EBufferType::Default, false);
    b->CreateSRV(&heap, heap.GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength));
    b->CreateUAV(&heap, heap.GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength));
    return b;
}
template<class T> ComPtr<T> Open(GDX12Device& owner, T* object, GDX12Device& other) {
    HANDLE handle = nullptr;
    ThrowIfFailed(owner.GetDevice()->CreateSharedHandle(object, nullptr, GENERIC_ALL, nullptr, &handle));
    ComPtr<T> imported;
    const auto hr = other.GetDevice()->OpenSharedHandle(handle, IID_PPV_ARGS(&imported));
    CloseHandle(handle); ThrowIfFailed(hr); return imported;
}
Id PacketFor(const Schedule& schedule, Id op) {
    for (Id i = 0; i < schedule.Packets.size(); ++i) if (schedule.Packets[i].Operation == op) return i;
    throw std::logic_error("water operation missing from schedule");
}
}
struct WaterTransfer::Impl {
    struct Instance {
        GDX12FluidSolver::Buffers Solver;
        GDX12FluidSolver::Head Head;
        uint64_t Steps = 0;
        std::unique_ptr<GDX12UploadBuffer<GDX12FluidParameters>> Constants;
        std::array<std::unique_ptr<GDX12UploadBuffer<XMFLOAT3>>, Slots> Packed, Received;
        std::unique_ptr<GDX12UploadBuffer<GDX12FluidSolver::Particle>> Display, Seed;
        uint64_t Offset = 0;
        bool SeedPending = true;
    };
    struct StepInput {
        GDX12FluidInstance Input;
        GDX12FluidSolver::Steps Batch;
        uint64_t FirstStep = 0, LastStep = 0;
    };
    struct Publication {
        uint64_t Sequence = 0, Generation = 0, Epoch = 0, Incarnation = 0, Bytes = 0;
        std::vector<StepInput> Inputs;
    };
    struct PackedSlot { CompletionToken Retired; };
    struct SharedSlot {
        ComPtr<ID3D12Heap> SecondaryHeap, PrimaryHeap;
        ComPtr<ID3D12Resource> Secondary, Primary;
        Publication Metadata;
        CompletionToken Ready, Imported;
        uint64_t Generation = 0;
        bool Occupied = false, Writing = false, Claimed = false;
    };
    struct ReceivedSlot {
        Publication Metadata;
        CompletionToken Imported, History;
        bool Pinned = false;
    };
    GDX12Device& Primary;
    GDX12DescriptorHeap& PrimaryHeap;
    GDX12Device Secondary;
    std::shared_ptr<QueueSubmission> Submit;
    std::unique_ptr<Executor> Import;
    std::unique_ptr<GDX12DescriptorHeap> Heap;
    GDX12FluidSolver Solver;
    std::unique_ptr<GDX12RootSignature> PackRoot, DisplayRoot;
    ComPtr<ID3D12PipelineState> PackPSO, DisplayPSO, RepeatPSO;
    std::vector<std::unique_ptr<Instance>> Instances;
    std::vector<GDX12FluidInstance> Inputs;
    std::vector<GDX12FluidInstance> Topology;
    std::array<PackedSlot, Slots> Packed;
    std::array<SharedSlot, Slots> Shared;
    std::array<ReceivedSlot, Slots> Received;
    std::array<CompletionToken, 3> FrameTails{};
    Publication Displayed;
    uint64_t Bytes = 0, Sequence = 0, Epoch = 0;
    Id Current = Invalid, Pending = Invalid, FrameReceived = Invalid, HistoryOp = Invalid;
    bool Running = false;
    std::thread Worker;
    std::atomic<bool> Cancel{false};
    mutable std::mutex Mutex;
    std::condition_variable Wake;
    std::exception_ptr Failure;
    std::thread::id Owner = std::this_thread::get_id();

    Impl(GDX12Device& primary, GDX12DescriptorHeap& primaryHeap, int adapter)
        : Primary(primary), PrimaryHeap(primaryHeap) {
        if (adapter < 0) throw std::invalid_argument("Meta.Water requires an explicit secondary adapter");
        ComPtr<IDXGIFactory6> factory; ThrowIfFailed(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
        ComPtr<IDXGIAdapter1> a, b;
        ThrowIfFailed(factory->EnumAdapterByLuid(primary.GetAdapterLuid(), IID_PPV_ARGS(&a)));
        ThrowIfFailed(factory->EnumAdapters1(UINT(adapter), &b));
        DXGI_ADAPTER_DESC1 da{}, db{}; ThrowIfFailed(a->GetDesc1(&da)); ThrowIfFailed(b->GetDesc1(&db));
        if ((da.Flags | db.Flags) & DXGI_ADAPTER_FLAG_SOFTWARE ||
            (da.AdapterLuid.HighPart == db.AdapterLuid.HighPart && da.AdapterLuid.LowPart == db.AdapterLuid.LowPart))
            throw std::invalid_argument("Meta.Water requires two distinct hardware adapters");
        ComPtr<IDXGIAdapter4> selected; ThrowIfFailed(b.As(&selected));
        ThrowIfFailed(Secondary.Initialize(selected));
        Submit = std::make_shared<QueueSubmission>(Primary, &Secondary);
        Import = std::make_unique<Executor>(Submit, 0, Slots, 6, 2);
        Heap = std::make_unique<GDX12DescriptorHeap>(&Secondary, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
            TextureResources_StartIndex + TextureResources_RangeLength, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
        GDX12RootSignatureDesc root;
        root.VolatileSRVData = true; root.Constants = {2}; root.NumSingleSRVSlots = 2; root.NumSingleUAVSlots = 2;
        PackRoot = std::make_unique<GDX12RootSignature>(&Secondary, root);
        DisplayRoot = std::make_unique<GDX12RootSignature>(&Primary, root);
        Solver.Initialize(&Secondary);
        PackPSO = GDX12RenderHelpers::CreateComputePipeline(&Secondary, PackRoot.get(), APP_SHADERS_FOLDER "WaterTransfer.hlsl", "Pack");
        DisplayPSO = GDX12RenderHelpers::CreateComputePipeline(&Primary, DisplayRoot.get(), APP_SHADERS_FOLDER "WaterTransfer.hlsl", "AdvanceDisplay");
        RepeatPSO = GDX12RenderHelpers::CreateComputePipeline(&Primary, DisplayRoot.get(), APP_SHADERS_FOLDER "WaterTransfer.hlsl", "RepeatDisplay");
        Log::Info("MetaRenderGraph", "Water domain primary LUID=", da.AdapterLuid.HighPart, ":", da.AdapterLuid.LowPart,
            " secondary index=", adapter, " LUID=", db.AdapterLuid.HighPart, ":", db.AdapterLuid.LowPart,
            " incarnation=", Submit->Incarnation());
    }
    void CheckOwner() const {
        if (Owner != std::this_thread::get_id()) throw std::logic_error("water host requires primary owner thread");
    }
    void Check(const Publication& m) const {
        if (!m.Sequence || !m.Generation || m.Epoch != Epoch || m.Incarnation != Submit->Incarnation() ||
            m.Bytes != Bytes || m.Inputs.size() != Instances.size()) throw std::logic_error("stale water publication");
        for (size_t i = 0; i < Instances.size(); ++i) {
            const auto& step = m.Inputs[i];
            if (step.Input.Id != Topology[i].Id || step.Input.Component.GetParticles() != Topology[i].Component.GetParticles() ||
                step.Input.Component.GetResetRevision() != Topology[i].Component.GetResetRevision() ||
                step.LastStep < step.FirstStep || step.LastStep - step.FirstStep != step.Batch.Count)
                throw std::logic_error("water publication input/step identity mismatch");
        }
    }
    void Configure(bool preserve) {
        if (Epoch == UINT64_MAX) throw std::overflow_error("water epoch exhausted");
        // Capacity is frozen by the live scene topology, not an unbounded work queue.
        if (Inputs.size() > TextureResources_RangeLength / 16) throw std::length_error("water topology exceeds solver descriptor range");
        std::vector<std::unique_ptr<Instance>> retained(Inputs.size());
        if (preserve) for (size_t i = 0; i < Inputs.size(); ++i)
            for (size_t j = 0; j < Topology.size(); ++j)
                if (Inputs[i].Id == Topology[j].Id && Inputs[i].Component.GetParticles() == Topology[j].Component.GetParticles() &&
                    Inputs[i].Component.GetResetRevision() == Topology[j].Component.GetResetRevision())
                    retained[i] = std::move(Instances[j]);
        Instances.clear(); // release removed/reset descriptors before allocating replacements
        ++Epoch; Bytes = 0; Topology = Inputs;
        for (size_t i = 0; i < Inputs.size(); ++i) {
            const auto& input = Inputs[i];
            if (retained[i]) {
                retained[i]->Offset = Bytes;
                Bytes += uint64_t(retained[i]->Solver.Count) * sizeof(XMFLOAT3);
                Instances.push_back(std::move(retained[i]));
                continue;
            }
            auto item = std::make_unique<Instance>();
            item->Offset = Bytes;
            item->Solver = GDX12FluidSolver::CreateBuffers(&Secondary, Heap.get(), input);
            const auto count = item->Solver.Count;
            Bytes += uint64_t(count) * sizeof(XMFLOAT3);
            item->Constants = std::make_unique<GDX12UploadBuffer<GDX12FluidParameters>>(&Secondary, Slots);
            for (UINT slot = 0; slot < Slots; ++slot) {
                item->Packed[slot] = Buffer<XMFLOAT3>(Secondary, *Heap, count);
                item->Received[slot] = Buffer<XMFLOAT3>(Primary, PrimaryHeap, count);
            }
            item->Display = Buffer<GDX12FluidSolver::Particle>(Primary, PrimaryHeap, count);
            item->Seed = std::make_unique<GDX12UploadBuffer<GDX12FluidSolver::Particle>>(&Primary, count, EBufferType::Upload, false);
            const auto seed = GDX12FluidSolver::MakeSeed(input);
            item->Seed->CopyData(0, seed.data(), count);
            Instances.push_back(std::move(item));
        }
        if (!Bytes) return;
        const auto desc = CD3DX12_RESOURCE_DESC::Buffer(Bytes, D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER);
        const auto x = Secondary.GetDevice()->GetResourceAllocationInfo(0, 1, &desc);
        const auto y = Primary.GetDevice()->GetResourceAllocationInfo(0, 1, &desc);
        if (x.SizeInBytes == UINT64_MAX || y.SizeInBytes == UINT64_MAX) throw std::runtime_error("water sharing unsupported");
        D3D12_HEAP_DESC heap{};
        heap.SizeInBytes = (std::max)(x.SizeInBytes, y.SizeInBytes); heap.Alignment = (std::max)(x.Alignment, y.Alignment);
        heap.Properties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        heap.Flags = D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER;
        for (auto& s : Shared) {
            ThrowIfFailed(Secondary.GetDevice()->CreateHeap(&heap, IID_PPV_ARGS(&s.SecondaryHeap)));
            s.PrimaryHeap = Open(Secondary, s.SecondaryHeap.Get(), Primary);
            ThrowIfFailed(Secondary.GetDevice()->CreatePlacedResource(s.SecondaryHeap.Get(), 0, &desc,
                D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&s.Secondary)));
            ThrowIfFailed(Primary.GetDevice()->CreatePlacedResource(s.PrimaryHeap.Get(), 0, &desc,
                D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&s.Primary)));
            Submit->ValidateResource(0, s.Primary.Get()); Submit->ValidateResource(1, s.Secondary.Get());
        }
        Log::Info("MetaRenderGraph", "Water epoch=", Epoch, " instances=", Inputs.size(), " position bytes=", Bytes,
            " packed/shared/received slots=", Slots);
    }
    void Start() {
        if (Running || Instances.empty()) return;
        if (Failure) std::rethrow_exception(Failure);
        Submit->CheckHealth(); Cancel.store(false);
        Worker = std::thread([this] { Produce(); }); Running = true;
    }
    void Stop() {
        Cancel.store(true); Wake.notify_all();
        if (Worker.joinable()) Worker.join();
        Submit->CheckHealth(); Import->Drain();
        for (auto token : FrameTails) if (token) Submit->Wait(token);
        for (auto p : Packed) if (p.Retired) Submit->Wait(p.Retired);
        for (auto& s : Shared) if (s.Ready) Submit->Wait(s.Ready);
        for (auto& r : Received) {
            if (r.Imported) Submit->Wait(r.Imported);
            if (r.History) Submit->Wait(r.History);
            if (r.Pinned) throw std::logic_error("water received lease missing retirement evidence");
        }
        if (HistoryOp != Invalid) throw std::logic_error("water history missing frame retirement evidence");
        // Only joined AND drained producer reservations may be abandoned.
        for (auto& s : Shared) if (s.Writing) {
            s.Writing = s.Occupied = s.Claimed = false; s.Ready = {}; s.Imported = {}; s.Metadata = {};
        }
        Running = false;
    }
    void Produce() {
        try {
            Executor context(Submit, 1, Slots, 9, 2);
            auto previous = std::chrono::steady_clock::now();
            while (!Cancel.load()) {
                Id packed = Invalid, shared = Invalid;
                Publication input;
                {
                    std::unique_lock<std::mutex> lock(Mutex);
                    for (Id i = 0; i < Slots; ++i) {
                        auto& s = Shared[i];
                        if (s.Occupied && s.Claimed && s.Imported && Submit->Complete(s.Imported)) s.Occupied = false;
                        if (shared == Invalid && !s.Occupied && !s.Writing) shared = i;
                        if (packed == Invalid && (!Packed[i].Retired || Submit->Complete(Packed[i].Retired)) && context.TryBegin(i)) packed = i;
                    }
                    if (packed == Invalid) { Wake.wait_for(lock, std::chrono::milliseconds(1)); continue; }
                    const auto now = std::chrono::steady_clock::now();
                    const float elapsed = std::chrono::duration<float>(now - previous).count();
                    previous = now;
                    bool work = false;
                    for (size_t i = 0; i < Inputs.size(); ++i) {
                        auto value = Inputs[i];
                        value.DeltaSeconds = elapsed * value.ClockScale;
                        const auto& settings = value.Component.GetSettings();
                        const auto steps = GDX12FluidSolver::PlanSteps(settings, value.DeltaSeconds * settings.TimeScale);
                        const auto first = Instances[i]->Steps;
                        if (UINT64_MAX - first < steps.Count) throw std::overflow_error("water step identity exhausted");
                        input.Inputs.push_back({value, steps, first, first + steps.Count});
                        work |= Instances[i]->Head.SeedPending || steps.Count != 0;
                    }
                    if (!work) { Wake.wait_for(lock, std::chrono::milliseconds(1)); continue; }
                    if (Sequence == UINT64_MAX) throw std::overflow_error("water publication sequence exhausted");
                    input.Sequence = Sequence + 1; input.Epoch = Epoch; input.Incarnation = Submit->Incarnation(); input.Bytes = Bytes;
                    if (shared != Invalid) {
                        auto& s = Shared[shared];
                        if (s.Generation == UINT64_MAX) throw std::overflow_error("water generation exhausted");
                        input.Generation = ++s.Generation; s.Writing = true;
                    }
                }
                Graph graph; graph.Device = 1; graph.FrameBoundary = false; graph.TerminalQueue = Queue::Copy;
                std::vector<ID3D12Resource*> bindings;
                std::vector<Access> solve, pack, exportAccess;
                std::vector<GDX12RecordingStates::Binding> solveStates, packStates;
                const auto add = [&](GDX12Resource& r, Mode mode, std::vector<Access>& accesses,
                                     std::vector<GDX12RecordingStates::Binding>& states, bool upload = false) {
                    const Id id = Id(graph.Resources.size());
                    const auto state = upload ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                    graph.Resources.push_back({"water secondary buffer", upload ? State(state) : 0, upload ? State(state) : 0,
                        !upload, !upload, upload ? Meta::Heap::Upload : Meta::Heap::Default});
                    bindings.push_back(r.D3DResource.Get()); accesses.push_back({id, mode, State(state)});
                    states.emplace_back(r.D3DResource.Get(), state); return id;
                };
                std::vector<Id> packedIds;
                std::vector<GDX12FluidSolver::Head> heads;
                for (size_t i = 0; i < Instances.size(); ++i) {
                    auto& item = *Instances[i];
                    item.Constants->CopyData(packed, MakeFluidParameters(input.Inputs[i].Input.ContainerToWorld,
                        input.Inputs[i].Input.Component.GetSettings()));
                    std::array<Id, 2> particles;
                    for (UINT p = 0; p < 2; ++p) particles[p] = add(item.Solver.Particles[p]->GetResource(), Mode::ReadWrite, solve, solveStates);
                    add(item.Solver.Density->GetResource(), Mode::ReadWrite, solve, solveStates);
                    add(item.Solver.Keys->GetResource(), Mode::ReadWrite, solve, solveStates);
                    add(item.Solver.Offsets->GetResource(), Mode::ReadWrite, solve, solveStates);
                    add(item.Constants->GetResource(), Mode::Read, solve, solveStates, true);
                    add(item.Solver.SeedUpload->GetResource(), Mode::Read, solve, solveStates, true);
                    const auto current = (item.Head.Current + input.Inputs[i].Batch.Count) % 2;
                    pack.push_back({particles[current], Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE});
                    packStates.emplace_back(item.Solver.Particles[current]->GetResource().D3DResource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    packedIds.push_back(add(item.Packed[packed]->GetResource(), Mode::Write, pack, packStates));
                    heads.push_back(item.Head);
                }
                graph.Operations.push_back({"water solver seed/SPH", solve, {}, false, 1, true, Queue::Compute});
                graph.Operations.push_back({"water pack float3 positions", pack, {0}, false, 1, true, Queue::Compute});
                if (shared != Invalid) {
                    for (auto id : packedIds) exportAccess.push_back({id, Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE});
                    const Id id = Id(graph.Resources.size());
                    graph.Resources.push_back({"water secondary shared", 0, 0, false, true}); bindings.push_back(Shared[shared].Secondary.Get());
                    exportAccess.push_back({id, Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST});
                    graph.Operations.push_back({"water COPY export", exportAccess, {1}, false, 1, false, Queue::Copy});
                }
                const auto schedule = Compile(graph);
                context.Execute(schedule, bindings, [&](Id op, uint32_t, ID3D12GraphicsCommandList* cmd) {
                    if (op == 2) {
                        Check(input);
                        for (auto& item : Instances) cmd->CopyBufferRegion(Shared[shared].Secondary.Get(), item->Offset,
                            item->Packed[packed]->GetResource().D3DResource.Get(), 0, uint64_t(item->Solver.Count) * sizeof(XMFLOAT3));
                        return;
                    }
                    GDX12RecordingStates tracker(op == 0 ? solveStates : packStates);
                    GDX12CommandList borrowed(cmd); borrowed.SetDescriptorHeaps({Heap.get()});
                    for (size_t i = 0; i < Instances.size(); ++i) {
                        auto& item = *Instances[i]; const auto& step = input.Inputs[i];
                        if (op == 0) heads[i] = Solver.Record(borrowed, item.Solver, item.Head,
                            step.Input.Component.GetSettings(), step.Batch, item.Constants->GetElementAddress(packed));
                        else {
                            auto& current = *item.Solver.Particles[heads[i].Current];
                            borrowed.ResourceBarrier({current.GetResource().GetNonPixelShaderResourceBarrier(),
                                item.Packed[packed]->GetResource().GetUnorderedAccessBarrier()});
                            borrowed.SetComputeRootSignature(PackRoot.get());
                            const UINT constants[] = {item.Solver.Count, 0}; cmd->SetComputeRoot32BitConstants(0, 2, constants, 0);
                            borrowed.SetComputeSRV(0, current.GetSRV()->GPUHandle);
                            borrowed.SetComputeUAV(0, item.Packed[packed]->GetUAV()->GPUHandle);
                            borrowed.SetPipelineState(PackPSO); borrowed.Dispatch((item.Solver.Count + 127) / 128, 1, 1);
                            borrowed.ResourceBarrier({item.Packed[packed]->GetResource().GetUAVBarrier()});
                        }
                    }
                    tracker.Finish(cmd);
                }, nullptr, nullptr, false);
                const auto retired = context.Completions().at(PacketFor(schedule, shared == Invalid ? 1 : 2));
                {
                    std::lock_guard<std::mutex> lock(Mutex);
                    Packed[packed].Retired = retired; Sequence = input.Sequence;
                    for (size_t i = 0; i < Instances.size(); ++i) { Instances[i]->Head = heads[i]; Instances[i]->Steps = input.Inputs[i].LastStep; }
                    if (shared != Invalid) {
                        auto& s = Shared[shared]; s.Metadata = std::move(input); s.Ready = retired; s.Imported = {};
                        s.Claimed = false; s.Occupied = true; s.Writing = false;
                    }
                }
            }
            context.Drain();
        } catch (...) {
            std::lock_guard<std::mutex> lock(Mutex);
            // Stop finishes the active batch instead of canceling half a solver
            // step. Any failure requires recreation, even during shutdown.
            Failure = std::current_exception();
            Cancel.store(true); Wake.notify_all();
        }
    }
    bool FreeReceived(Id i) const {
        const auto& r = Received[i];
        return i != Current && i != Pending && !r.Pinned && (!r.Imported || Submit->Complete(r.Imported)) &&
            (!r.History || Submit->Complete(r.History));
    }
    void Select(PublicationSelection mode) {
        if (Pending != Invalid && Submit->Complete(Received[Pending].Imported)) { Current = Pending; Pending = Invalid; }
        if (Pending == Invalid) {
            Id target = Invalid, source = Invalid;
            for (Id i = 0; i < Slots; ++i) if (FreeReceived(i)) { target = i; break; }
            const auto previous = Current == Invalid ? 0 : Received[Current].Metadata.Sequence;
            if (target != Invalid) for (Id i = 0; i < Slots; ++i) {
                auto& s = Shared[i];
                if (!s.Occupied || s.Writing || s.Claimed || s.Metadata.Sequence <= previous) continue;
                Check(s.Metadata);
                if (mode == PublicationSelection::LatestCompleted && !Submit->Complete(s.Ready)) continue;
                if (source == Invalid || s.Metadata.Sequence > Shared[source].Metadata.Sequence) source = i;
            }
            if (source != Invalid && Import->TryBegin(target)) {
                auto& s = Shared[source]; auto& r = Received[target];
                if (s.Metadata.Generation != s.Generation) throw std::logic_error("water shared generation mismatch");
                Graph graph; graph.FrameBoundary = false; graph.TerminalQueue = Queue::Copy;
                graph.Resources.push_back({"water primary shared", 0, 0, false, true});
                std::vector<ID3D12Resource*> bindings{s.Primary.Get()};
                std::vector<Access> accesses{{0, Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE}};
                for (auto& item : Instances) {
                    const Id id = Id(graph.Resources.size()); graph.Resources.push_back({"water received positions", 0, 0, true, true});
                    bindings.push_back(item->Received[target]->GetResource().D3DResource.Get());
                    accesses.push_back({id, Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST});
                }
                graph.Operations.push_back({"water COPY import", accesses, {}, false, 1, false, Queue::Copy});
                const auto schedule = Compile(graph); s.Claimed = true;
                Import->Execute(schedule, bindings, [&](Id, uint32_t, ID3D12GraphicsCommandList* cmd) {
                    Check(s.Metadata);
                    for (auto& item : Instances) cmd->CopyBufferRegion(item->Received[target]->GetResource().D3DResource.Get(), 0,
                        s.Primary.Get(), item->Offset, uint64_t(item->Solver.Count) * sizeof(XMFLOAT3));
                }, nullptr, nullptr, false, nullptr, nullptr, {{PacketFor(schedule, 0), s.Ready}});
                r.Metadata = s.Metadata; r.Imported = s.Imported = Import->Completions().at(PacketFor(schedule, 0));
                r.History = {}; Pending = target;
                for (auto& old : Shared) if (old.Occupied && !old.Claimed && !old.Writing &&
                    old.Metadata.Sequence < s.Metadata.Sequence && Submit->Complete(old.Ready)) old.Occupied = false;
            }
        }
        FrameReceived = mode == PublicationSelection::StrictPublication && Pending != Invalid ? Pending : Current;
        // A preceding Strict frame may already have queued a newer display than
        // Current. DIRECT FIFO preserves it; Latest must not import an older one.
        if (FrameReceived != Invalid && Received[FrameReceived].Metadata.Sequence <= Displayed.Sequence) FrameReceived = Invalid;
        if (FrameReceived != Invalid) { Check(Received[FrameReceived].Metadata); Received[FrameReceived].Pinned = true; }
    }
};

WaterTransfer::WaterTransfer(GDX12Device& primary, GDX12DescriptorHeap& heap, int adapter)
    : _impl(std::make_unique<Impl>(primary, heap, adapter)) {}
WaterTransfer::~WaterTransfer() {
    try { Stop(); } catch (...) {
        _impl->Cancel.store(true); _impl->Wake.notify_all();
        if (_impl->Worker.joinable()) _impl->Worker.join();
        OutputDebugStringA("Meta water drain failed; retaining complete device/heap/context domain\n");
        _impl.release();
    }
}
const std::shared_ptr<QueueSubmission>& WaterTransfer::Submissions() const { return _impl->Submit; }
void WaterTransfer::Stop() { _impl->CheckOwner(); _impl->Stop(); }
void WaterTransfer::Update(const std::vector<GDX12FluidInstance>& inputs, bool reset) {
    auto& p = *_impl; p.CheckOwner();
    std::set<uint64_t> ids;
    for (const auto& input : inputs) {
        ValidateFluidInstance(input);
        if (!input.Component.GetParticles() || input.Component.GetParticles()->empty() || !ids.insert(input.Id).second)
            throw std::invalid_argument("water domain requires unique nonempty instances");
    }
    bool changed = reset;
    {
        std::lock_guard<std::mutex> lock(p.Mutex);
        if (p.Failure) std::rethrow_exception(p.Failure);
        changed |= p.Inputs.size() != inputs.size();
        if (!changed) for (size_t i = 0; i < inputs.size(); ++i)
            changed |= p.Inputs[i].Id != inputs[i].Id || p.Inputs[i].Component.GetParticles() != inputs[i].Component.GetParticles() ||
                p.Inputs[i].Component.GetResetRevision() != inputs[i].Component.GetResetRevision();
        if (!changed) p.Inputs = inputs;
    }
    if (changed) {
        p.Stop();
        p.Shared = {}; p.Packed = {}; p.Received = {}; p.FrameTails = {};
        p.Import = std::make_unique<Executor>(p.Submit, 0, Slots, 6, 2);
        p.Inputs = inputs; p.Current = p.Pending = p.FrameReceived = Invalid; p.Sequence = 0; p.Displayed = {};
        p.Configure(!reset);
    }
    p.Start(); p.Wake.notify_all();
}
GDX12FluidSurfacePass::ExternalDisplay WaterTransfer::Displays() const {
    const auto& p = *_impl; p.CheckOwner();
    GDX12FluidSurfacePass::ExternalDisplay result;
    for (size_t i = 0; i < p.Instances.size(); ++i) result.emplace(p.Inputs[i].Id, p.Instances[i]->Display.get());
    return result;
}
void WaterTransfer::Append(Graph& graph, std::vector<ID3D12Resource*>& bindings, PublicationSelection mode) {
    auto& p = *_impl; p.CheckOwner();
    if (mode != PublicationSelection::StrictPublication && mode != PublicationSelection::LatestCompleted)
        throw std::invalid_argument("unknown water publication selection");
    if (graph.Device != 0 || bindings.size() != graph.Resources.size() || p.HistoryOp != Invalid)
        throw std::logic_error("invalid water primary graph lifecycle");
    std::lock_guard<std::mutex> lock(p.Mutex);
    if (p.Failure) std::rethrow_exception(p.Failure);
    p.Select(mode);
    if (p.Instances.empty()) return;
    std::vector<Access> accesses;
    const auto add = [&](GDX12Resource& r, Mode usage, State state) {
        const Id id = NativeWork::ResourceFor(graph, bindings, r.D3DResource.Get(), "water primary buffer");
        accesses.push_back({id, usage, state});
    };
    for (auto& item : p.Instances) {
        add(item->Display->GetResource(), Mode::ReadWrite, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (item->SeedPending) add(item->Seed->GetResource(), Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ);
        if (p.FrameReceived != Invalid) add(item->Received[p.FrameReceived]->GetResource(), Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    p.HistoryOp = Id(graph.Operations.size());
    graph.Operations.push_back({"water previous displayed positions / seed / advance", accesses, {}, false, 1, true, Queue::Direct});
}
bool WaterTransfer::Record(Id operation, ID3D12GraphicsCommandList* cmd) {
    auto& p = *_impl;
    if (operation == Invalid || operation != p.HistoryOp) return false;
    std::vector<GDX12RecordingStates::Binding> states;
    for (auto& item : p.Instances) {
        states.emplace_back(item->Display->GetResource().D3DResource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (item->SeedPending) states.emplace_back(item->Seed->GetResource().D3DResource.Get(), D3D12_RESOURCE_STATE_GENERIC_READ);
        if (p.FrameReceived != Invalid) states.emplace_back(item->Received[p.FrameReceived]->GetResource().D3DResource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    GDX12RecordingStates tracker(states); GDX12CommandList borrowed(cmd);
    borrowed.SetDescriptorHeaps({&p.PrimaryHeap});
    for (auto& item : p.Instances) {
        auto& display = item->Display->GetResource();
        if (item->SeedPending) {
            borrowed.ResourceBarrier({display.GetCopyDestBarrier()});
            borrowed.CopyResource(display.D3DResource.Get(), item->Seed->GetResource().D3DResource.Get());
        }
        borrowed.ResourceBarrier({display.GetUnorderedAccessBarrier()});
        borrowed.SetComputeRootSignature(p.DisplayRoot.get());
        const UINT constants[] = {item->Solver.Count, p.FrameReceived != Invalid ? 1u : 0u};
        cmd->SetComputeRoot32BitConstants(0, 2, constants, 0);
        if (p.FrameReceived != Invalid) borrowed.SetComputeSRV(1, item->Received[p.FrameReceived]->GetSRV()->GPUHandle);
        borrowed.SetComputeUAV(1, item->Display->GetUAV()->GPUHandle);
        borrowed.SetPipelineState(p.FrameReceived != Invalid ? p.DisplayPSO : p.RepeatPSO);
        borrowed.Dispatch((item->Solver.Count + 127) / 128, 1, 1);
        borrowed.ResourceBarrier({display.GetUAVBarrier()});
    }
    tracker.Finish(cmd); return true;
}
std::vector<PacketWait> WaterTransfer::Waits(const Schedule& schedule) const {
    const auto& p = *_impl;
    if (p.HistoryOp == Invalid || p.FrameReceived == Invalid) return {};
    return {{PacketFor(schedule, p.HistoryOp), p.Received[p.FrameReceived].Imported}};
}
void WaterTransfer::RetireFrame(const Schedule& schedule, const std::vector<CompletionToken>& tokens, bool success) {
    auto& p = *_impl; p.CheckOwner();
    if (tokens.size() != schedule.Packets.size()) throw std::logic_error("water frame retirement evidence missing");
    // Track all primary frames, including empty-water frames: world reset also
    // replaces the scene adapter's particle pools and targets.
    for (auto token : tokens) if (token) {
        if (token.Device != 0) throw std::logic_error("foreign water primary reader token");
        p.Submit->Complete(token);
        const auto q = token.Target == Queue::Direct ? 0 : token.Target == Queue::Compute ? 1 : 2;
        if (token.Value > p.FrameTails[q].Value) p.FrameTails[q] = token;
    }
    if (p.HistoryOp == Invalid) return;
    const auto token = tokens.at(PacketFor(schedule, p.HistoryOp));
    if (success && !token) throw std::logic_error("water history has no issued completion");
    if (p.FrameReceived != Invalid) {
        auto& r = p.Received[p.FrameReceived];
        if (token) r.History = token;
        r.Pinned = false;
        if (success) p.Displayed = r.Metadata;
    }
    if (success) for (auto& item : p.Instances) item->SeedPending = false;
    p.FrameReceived = p.HistoryOp = Invalid; p.Wake.notify_all();
}
}

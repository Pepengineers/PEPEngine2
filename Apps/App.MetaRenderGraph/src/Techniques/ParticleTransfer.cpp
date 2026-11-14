#include "ParticleTransfer.h"
#include "NativeWork.h"
#include "../../../../Engine/Engine.RendererDX12/src/GDX12ParticleOffload.h"
#include "Engine.Core/Logging/Logger.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <thread>

namespace Meta {
namespace {
constexpr UINT Slots = 3;
Id PacketFor(const Schedule& schedule, Id operation) {
    for (Id i = 0; i < schedule.Packets.size(); ++i)
        if (schedule.Packets[i].Operation == operation) return i;
    throw std::logic_error("particle operation missing from schedule");
}
template<class T> ComPtr<T> Open(GDX12Device& owner, T* object, GDX12Device& other) {
    HANDLE handle = nullptr;
    ThrowIfFailed(owner.GetDevice()->CreateSharedHandle(object, nullptr, GENERIC_ALL, nullptr, &handle));
    ComPtr<T> result;
    const auto hr = other.GetDevice()->OpenSharedHandle(handle, IID_PPV_ARGS(&result));
    CloseHandle(handle); ThrowIfFailed(hr); return result;
}
}
struct ParticleTransfer::Impl {
    using Entry = GDX12ParticleOffload::Entry;
    template<class T> using Buffer = ParticleResources::Buffer<T>;
    struct Exchange {
        ComPtr<ID3D12Heap> SecondaryHeap, PrimaryHeap;
        ComPtr<ID3D12Resource> Secondary, Primary;
    };
    struct SharedSlot : Exchange {
        uint64_t Sequence = 0;
        CompletionToken Ready, Imported;
        bool Occupied = false, Claimed = false, Writing = false;
    };
    struct ReceivedSlot {
        std::unique_ptr<Buffer<UINT>> Data;
        uint64_t Sequence = 0;
        CompletionToken Imported, History;
        bool Pinned = false;
    };
    struct InputSlot : Exchange {
        GDX12ParticleInteractionField Field;
        uint64_t Sequence = 0;
        CompletionToken Ready, Consumed;
        bool Pinned = false, Claimed = false;
    };
    std::shared_ptr<SimulationDomain> Domain;
    std::shared_ptr<QueueSubmission> Submit;
    GDX12DeviceResources& Primary;
    GDX12DeviceResources Resources;
    GDX12ParticleSimulation Simulation;
    RenderPipelineCommonData Common;
    std::unique_ptr<Executor> Import;
    std::unique_ptr<GDX12RootSignature> PackRoot, DisplayRoot;
    ComPtr<ID3D12PipelineState> PackPSO, DisplayPSO, RepeatPSO;
    std::array<std::unique_ptr<Buffer<UINT>>, Slots> Packed;
    std::array<CompletionToken, Slots> PackedRetired{};
    std::array<SharedSlot, Slots> Shared;
    std::array<ReceivedSlot, Slots> Received;
    std::array<InputSlot, Slots> Input;
    std::unique_ptr<Buffer<ParticleResources::Particle>> Display;
    std::unique_ptr<Buffer<XMFLOAT4>> Field;
    std::vector<Entry> Entries;
    std::vector<uint64_t> Ids;
    std::vector<GDX12ParticleEmitter> Inputs;
    std::unordered_map<uint64_t, uint64_t> ReturningBursts;
    GDX12ParticleInteractionField HostField;
    float ClockScale = 1;
    UINT Words = 0, Resolution = 0;
    uint64_t Epoch = 0, Sequence = 0, InputSequence = 0, Displayed = 0;
    Id Current = Invalid, Pending = Invalid, FrameReceived = Invalid, FrameInput = Invalid;
    Id HistoryOp = Invalid, InputOp = Invalid;
    bool SeedDisplay = true, ResetMotion = true;
    std::array<CompletionToken, 3> FrameTails{};
    std::atomic<bool> Cancel{false};
    std::thread Worker;
    std::mutex Mutex;
    std::condition_variable Wake;
    std::exception_ptr Failure;
    std::thread::id Owner = std::this_thread::get_id();

    Impl(std::shared_ptr<SimulationDomain> domain, GDX12DeviceResources& primary)
        : Domain(std::move(domain)), Submit(Domain->Submissions), Primary(primary) {
        Resources.Device = &Domain->Secondary;
        Resources.SRV_UAV_Heap = std::make_shared<GDX12DescriptorHeap>(Resources.Device,
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 131072, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
        Resources.FrameConstants.resize(Slots);
        Import = std::make_unique<Executor>(Submit, 0, Slots, 6, 2);
        GDX12RootSignatureDesc root;
        root.VolatileSRVData = true; root.Constants = {1, 1, 1, 16};
        root.NumSingleSRVSlots = 3; root.NumSingleUAVSlots = 2;
        PackRoot = std::make_unique<GDX12RootSignature>(Resources.Device, root);
        DisplayRoot = std::make_unique<GDX12RootSignature>(Primary.Device, root);
        PackPSO = GDX12RenderHelpers::CreateComputePipeline(Resources.Device, PackRoot.get(),
            SHADERS_FOLDER "ParticleTransfer.hlsl", "Pack");
        DisplayPSO = GDX12RenderHelpers::CreateComputePipeline(Primary.Device, DisplayRoot.get(),
            SHADERS_FOLDER "ParticleTransfer.hlsl", "AdvanceDisplay");
        RepeatPSO = GDX12RenderHelpers::CreateComputePipeline(Primary.Device, DisplayRoot.get(),
            SHADERS_FOLDER "ParticleTransfer.hlsl", "RepeatDisplay");
    }
    void CheckOwner() const {
        if (Owner != std::this_thread::get_id()) throw std::logic_error("particle host requires primary owner");
    }
    Exchange CreateExchange(uint64_t bytes) {
        auto& secondary = Domain->Secondary;
        const auto desc = CD3DX12_RESOURCE_DESC::Buffer(bytes, D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER);
        const auto a = secondary.GetDevice()->GetResourceAllocationInfo(0, 1, &desc);
        const auto b = Primary.Device->GetDevice()->GetResourceAllocationInfo(0, 1, &desc);
        if (a.SizeInBytes == UINT64_MAX || b.SizeInBytes == UINT64_MAX)
            throw std::runtime_error("particle cross-adapter sharing unsupported");
        D3D12_HEAP_DESC heap{};
        heap.SizeInBytes = (std::max)(a.SizeInBytes, b.SizeInBytes);
        heap.Alignment = (std::max)(a.Alignment, b.Alignment);
        heap.Properties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        heap.Flags = D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER;
        Exchange result;
        ThrowIfFailed(secondary.GetDevice()->CreateHeap(&heap, IID_PPV_ARGS(&result.SecondaryHeap)));
        result.PrimaryHeap = Open(secondary, result.SecondaryHeap.Get(), *Primary.Device);
        ThrowIfFailed(secondary.GetDevice()->CreatePlacedResource(result.SecondaryHeap.Get(), 0, &desc,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&result.Secondary)));
        ThrowIfFailed(Primary.Device->GetDevice()->CreatePlacedResource(result.PrimaryHeap.Get(), 0, &desc,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&result.Primary)));
        return result;
    }
    std::array<UINT, 16> Parameters(size_t index, uint64_t sequence) const {
        const auto& e = Entries[index];
        return {e.Capacity, e.Words, 8 + UINT(index) * 8, e.Base,
            UINT(e.Id), UINT(e.Id >> 32), UINT(e.ResetRevision), UINT(e.ResetRevision >> 32),
            UINT(Epoch), UINT(Epoch >> 32), UINT(sequence), UINT(sequence >> 32),
            Words, UINT(Entries.size()), 0, 0};
    }
    void Stop() {
        Cancel.store(true); Wake.notify_all();
        if (Worker.joinable()) Worker.join();
        Submit->CheckHealth(); Import->Drain();
        for (auto token : FrameTails) if (token) Submit->Wait(token);
        for (auto token : PackedRetired) if (token) Submit->Wait(token);
        for (auto& s : Shared) if (s.Ready) Submit->Wait(s.Ready);
        for (auto& r : Received) {
            if (r.Imported) Submit->Wait(r.Imported);
            if (r.History) Submit->Wait(r.History);
            if (r.Pinned) throw std::logic_error("particle display lease missing retirement");
        }
        for (auto& i : Input) {
            if (i.Ready) Submit->Wait(i.Ready);
            if (i.Consumed) Submit->Wait(i.Consumed);
            if (i.Pinned) throw std::logic_error("particle input lease missing retirement");
        }
        if (HistoryOp != Invalid || InputOp != Invalid) throw std::logic_error("particle frame missing retirement");
        if (Failure) std::rethrow_exception(Failure);
    }
    void Configure(const std::vector<GDX12ParticleEmitter>& emitters, UINT resolution,
                   const std::unordered_map<uint64_t, uint64_t>& burstSeeds, bool reset) {
        Stop();
        if (Epoch == UINT64_MAX) throw std::overflow_error("particle epoch exhausted");
        ++Epoch;
        // Topology changes are cold; unchanged solver pools retain their state.
        if (!Simulation._state || reset)
            Simulation._state = std::make_unique<GDX12ParticleSimulation::State>(&Resources, true);
        auto& state = *Simulation._state;
        auto oldPools = std::move(state.Pools);
        state.Active = true; state.ResetMotion = true;
        Entries.clear(); Ids.clear();
        if (emitters.size() > (UINT_MAX - 8) / 8) throw std::length_error("particle metadata too large");
        Words = 8 + UINT(emitters.size()) * 8;
        UINT total = 0;
        for (const auto& emitter : emitters) {
            if (emitter.MaxParticles > 4194304 - total || emitter.MaxParticles > (UINT_MAX / 4 - Words) / 9)
                throw std::length_error("particle batch too large");
            Entries.push_back({emitter.Id, emitter.ResetRevision, emitter.MaxParticles, Words, total});
            Ids.push_back(emitter.Id);
            Words += emitter.MaxParticles * 9; total += emitter.MaxParticles;
            const auto old = oldPools.find(emitter.Id);
            std::unique_ptr<ParticleResources::Pool> pool;
            if (old != oldPools.end() && old->second->Capacity == emitter.MaxParticles &&
                old->second->ResetRevision == emitter.ResetRevision) pool = std::move(old->second);
            else {
                pool = std::make_unique<ParticleResources::Pool>(&Resources, emitter.MaxParticles);
                pool->ResetRevision = emitter.ResetRevision;
                const auto seed = burstSeeds.find(emitter.Id);
                if (seed != burstSeeds.end()) pool->BurstTotal = seed->second;
            }
            state.Pools.emplace(emitter.Id, std::move(pool));
        }
        state.Capacity = total;
        Packed = {}; PackedRetired = {}; Shared = {}; Received = {}; Input = {}; FrameTails = {};
        Display.reset(); Field.reset(); Resolution = resolution;
        Current = Pending = FrameReceived = FrameInput = Invalid;
        Sequence = InputSequence = Displayed = 0; SeedDisplay = ResetMotion = true;
        Inputs = emitters;
        Import = std::make_unique<Executor>(Submit, 0, Slots, 6, 2);
        if (!total) return;
        Display = ParticleResources::CreateBuffer<ParticleResources::Particle>(&Primary, total);
        for (UINT i = 0; i < Slots; ++i) {
            Packed[i] = ParticleResources::CreateBuffer<UINT>(&Resources, Words);
            Received[i].Data = ParticleResources::CreateBuffer<UINT>(&Primary, Words);
            static_cast<Exchange&>(Shared[i]) = CreateExchange(uint64_t(Words) * 4);
            if (resolution) static_cast<Exchange&>(Input[i]) = CreateExchange(uint64_t(resolution) * resolution * sizeof(XMFLOAT4));
        }
        if (resolution) Field = ParticleResources::CreateBuffer<XMFLOAT4>(&Resources, resolution * resolution);
        Log::Info("MetaRenderGraph", "Particle batch epoch=", Epoch, " emitters=", Entries.size(),
            " payload bytes=", uint64_t(Words) * 4, "; one COPY export + one COPY import; shared domain=", Submit->Incarnation());
    }
    void Start() {
        if (Worker.joinable() || Entries.empty()) return;
        if (Failure) std::rethrow_exception(Failure);
        Cancel.store(false); Worker = std::thread([this] { Produce(); });
    }
    void Produce() {
        try {
            Executor context(Submit, 1, Slots, 12, 2);
            CompletionToken lastSolve;
            GDX12ParticleInteractionField field;
            uint64_t fieldSequence = 0;
            auto previous = std::chrono::steady_clock::now();
            while (!Cancel.load()) {
                Id packed = Invalid, shared = Invalid, inputSlot = Invalid;
                std::vector<GDX12ParticleEmitter> emitters;
                {
                    std::unique_lock<std::mutex> lock(Mutex);
                    for (Id i = 0; i < Slots; ++i) {
                        if (packed == Invalid && (!PackedRetired[i] || Submit->Complete(PackedRetired[i])) && context.TryBegin(i)) packed = i;
                        auto& s = Shared[i];
                        if (s.Occupied && s.Claimed && s.Imported && Submit->Complete(s.Imported)) s.Occupied = false;
                        if (shared == Invalid && !s.Occupied && !s.Writing) shared = i;
                        const auto& snapshot = Input[i];
                        if (snapshot.Ready && !snapshot.Pinned && snapshot.Sequence > fieldSequence &&
                            (inputSlot == Invalid || snapshot.Sequence > Input[inputSlot].Sequence)) inputSlot = i;
                    }
                    if (packed == Invalid || (Resolution && !fieldSequence && inputSlot == Invalid)) {
                        Wake.wait_for(lock, std::chrono::milliseconds(1)); continue;
                    }
                    if (inputSlot != Invalid) {
                        Input[inputSlot].Claimed = true;
                        field = Input[inputSlot].Field;
                        field.Resource = Field ? &Field->GetResource() : nullptr;
                        field.SRV = Field ? Field->GetSRV()->GPUHandle : D3D12_GPU_DESCRIPTOR_HANDLE{};
                    }
                    const auto now = std::chrono::steady_clock::now();
                    const float delta = std::chrono::duration<float>(now - previous).count() * ClockScale;
                    previous = now; emitters = Inputs;
                    for (auto& e : emitters) e.DeltaTime = delta;
                    if (shared != Invalid) Shared[shared].Writing = true;
                }
                if (Sequence == UINT64_MAX) throw std::overflow_error("particle sequence exhausted");
                const auto sequence = Sequence + 1;
                Resources.CurrFrameConstantsIndex = packed;
                Simulation._state->Emitters = std::move(emitters);
                Simulation.SetInteractionField(field);
                Graph graph; graph.Device = 1; graph.FrameBoundary = false; graph.TerminalQueue = Queue::Copy;
                std::vector<ID3D12Resource*> bindings;
                std::vector<Access> simAccess, packAccess;
                std::vector<GDX12RecordingStates::Binding> simStates, packStates;
                auto add = [&](GDX12Resource& resource, Mode usage, std::vector<Access>& accesses,
                               std::vector<GDX12RecordingStates::Binding>& states) {
                    const Id id = NativeWork::ResourceFor(graph, bindings, resource.D3DResource.Get(), "particle secondary buffer");
                    D3D12_HEAP_PROPERTIES heap{}; ThrowIfFailed(resource.D3DResource->GetHeapProperties(&heap, nullptr));
                    const auto state = heap.Type == D3D12_HEAP_TYPE_UPLOAD ?
                        D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                    accesses.push_back({id, usage, State(state)}); states.emplace_back(resource.D3DResource.Get(), state);
                    return id;
                };
                for (auto* r : Simulation.GetPoolResources()) add(*r, Mode::ReadWrite, simAccess, simStates);
                for (auto* r : Simulation.GetConstantResources(packed)) add(*r, Mode::Read, simAccess, simStates);
                if (Field) add(Field->GetResource(), Mode::Read, simAccess, simStates);
                Id inputOp = Invalid;
                if (Field && inputSlot != Invalid) {
                    const Id from = Id(graph.Resources.size());
                    graph.Resources.push_back({"particle shared interaction snapshot", 0, 0, false, true});
                    bindings.push_back(Input[inputSlot].Secondary.Get());
                    const Id to = NativeWork::ResourceFor(graph, bindings, Field->GetResource().D3DResource.Get(), "particle interaction");
                    inputOp = Id(graph.Operations.size());
                    graph.Operations.push_back({"particle interaction COPY import", {{from, Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE},
                        {to, Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST}}, {}, false, 1, false, Queue::Copy});
                }
                const Id solveOp = Id(graph.Operations.size());
                graph.Operations.push_back({"particle secondary simulate/death/spawn", simAccess, {}, false, 1, true, Queue::Compute});
                for (const auto& entry : Entries) {
                    const auto& pool = *Simulation._state->Pools.at(entry.Id);
                    add(pool.Particles->GetResource(), Mode::Read, packAccess, packStates);
                    add(pool.Counters->GetResource(), Mode::Read, packAccess, packStates);
                }
                const Id packedId = add(Packed[packed]->GetResource(), Mode::Write, packAccess, packStates);
                const Id packOp = Id(graph.Operations.size());
                graph.Operations.push_back({"particle common payload pack", packAccess, {solveOp}, false, 1, true, Queue::Compute});
                Id exportOp = Invalid;
                if (shared != Invalid) {
                    const Id to = Id(graph.Resources.size());
                    graph.Resources.push_back({"particle secondary shared payload", 0, 0, false, true});
                    bindings.push_back(Shared[shared].Secondary.Get());
                    exportOp = Id(graph.Operations.size());
                    graph.Operations.push_back({"particle one COPY export", {{packedId, Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE},
                        {to, Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST}}, {packOp}, false, 1, false, Queue::Copy});
                }
                const auto schedule = Compile(graph);
                std::vector<PacketWait> waits;
                if (inputOp != Invalid) {
                    waits.push_back({PacketFor(schedule, inputOp), Input[inputSlot].Ready});
                    if (lastSolve) waits.push_back({PacketFor(schedule, inputOp), lastSolve});
                }
                context.Execute(schedule, bindings, [&](Id op, uint32_t, ID3D12GraphicsCommandList* cmd) {
                    if (op == inputOp) {
                        cmd->CopyBufferRegion(Field->GetResource().D3DResource.Get(), 0, Input[inputSlot].Secondary.Get(), 0,
                            uint64_t(Resolution) * Resolution * sizeof(XMFLOAT4)); return;
                    }
                    if (op == exportOp) {
                        cmd->CopyBufferRegion(Shared[shared].Secondary.Get(), 0,
                            Packed[packed]->GetResource().D3DResource.Get(), 0, uint64_t(Words) * 4); return;
                    }
                    GDX12RecordingStates tracker(op == solveOp ? simStates : packStates);
                    GDX12CommandList borrowed(cmd); borrowed.SetDescriptorHeaps({Resources.SRV_UAV_Heap.get()});
                    if (op == solveOp) {
                        Simulation.InitializePools(&borrowed, &Resources);
                        Simulation.Execute(&borrowed, &Resources, Common, nullptr);
                    } else {
                        borrowed.SetComputeRootSignature(PackRoot.get()); borrowed.SetPipelineState(PackPSO);
                        ParticleResources::UAV(&borrowed, *Packed[packed]);
                        borrowed.SetComputeUAV(0, Packed[packed]->GetUAV()->GPUHandle);
                        for (size_t i = 0; i < Entries.size(); ++i) {
                            auto& pool = *Simulation._state->Pools.at(Entries[i].Id);
                            borrowed.SetComputeSRV(0, pool.Particles->GetSRV()->GPUHandle);
                            borrowed.SetComputeSRV(1, pool.Counters->GetSRV()->GPUHandle);
                            const auto parameters = Parameters(i, sequence);
                            cmd->SetComputeRoot32BitConstants(3, 16, parameters.data(), 0);
                            borrowed.Dispatch((Entries[i].Capacity + 255) / 256, 1, 1);
                        }
                        borrowed.ResourceBarrier({Packed[packed]->GetResource().GetUAVBarrier()});
                    }
                    tracker.Finish(cmd);
                }, nullptr, nullptr, false, nullptr, nullptr, waits);
                lastSolve = context.Completions().at(PacketFor(schedule, solveOp));
                const auto retired = context.Completions().at(PacketFor(schedule, exportOp == Invalid ? packOp : exportOp));
                {
                    std::lock_guard<std::mutex> lock(Mutex);
                    Sequence = sequence; PackedRetired[packed] = retired;
                    if (inputSlot != Invalid) {
                        auto& snapshot = Input[inputSlot];
                        snapshot.Consumed = inputOp == Invalid ? lastSolve : context.Completions().at(PacketFor(schedule, inputOp));
                        fieldSequence = snapshot.Sequence;
                    }
                    if (shared != Invalid) {
                        auto& s = Shared[shared];
                        s.Sequence = sequence; s.Ready = retired; s.Imported = {};
                        s.Occupied = true; s.Claimed = false; s.Writing = false;
                    }
                }
                Simulation._state->ResetMotion = false;
            }
            context.Drain();
        } catch (...) {
            std::lock_guard<std::mutex> lock(Mutex);
            Failure = std::current_exception(); Cancel.store(true); Wake.notify_all();
        }
    }
    void Select(PublicationSelection selection) {
        if (Pending != Invalid && Submit->Complete(Received[Pending].Imported)) { Current = Pending; Pending = Invalid; }
        if (Pending == Invalid) {
            Id target = Invalid, source = Invalid;
            for (Id i = 0; i < Slots; ++i) {
                const auto& r = Received[i];
                if (target == Invalid && i != Current && !r.Pinned &&
                    (!r.Imported || Submit->Complete(r.Imported)) && (!r.History || Submit->Complete(r.History))) target = i;
                const auto& s = Shared[i];
                if (!s.Occupied || s.Writing || s.Claimed || s.Sequence <= Displayed) continue;
                if (selection == PublicationSelection::LatestCompleted && !Submit->Complete(s.Ready)) continue;
                if (source == Invalid || s.Sequence > Shared[source].Sequence) source = i;
            }
            if (source != Invalid && target != Invalid && Import->TryBegin(target)) {
                auto& s = Shared[source]; auto& r = Received[target];
                Graph graph; graph.FrameBoundary = false; graph.TerminalQueue = Queue::Copy;
                graph.Resources = {{"particle primary shared payload", 0, 0, false, true},
                    {"particle received payload", 0, 0, true, true}};
                graph.Operations.push_back({"particle one COPY import", {{0, Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE},
                    {1, Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST}}, {}, false, 1, false, Queue::Copy});
                const auto schedule = Compile(graph);
                s.Claimed = true;
                Import->Execute(schedule, {s.Primary.Get(), r.Data->GetResource().D3DResource.Get()},
                    [&](Id, uint32_t, ID3D12GraphicsCommandList* cmd) {
                        cmd->CopyBufferRegion(r.Data->GetResource().D3DResource.Get(), 0, s.Primary.Get(), 0, uint64_t(Words) * 4);
                    }, nullptr, nullptr, false, nullptr, nullptr, {{PacketFor(schedule, 0), s.Ready}});
                r.Imported = s.Imported = Import->Completions().at(PacketFor(schedule, 0));
                r.Sequence = s.Sequence; r.History = {}; Pending = target;
                for (auto& old : Shared) if (old.Occupied && !old.Claimed && !old.Writing &&
                    old.Sequence < s.Sequence && Submit->Complete(old.Ready)) old.Occupied = false;
            }
        }
        FrameReceived = selection == PublicationSelection::StrictPublication && Pending != Invalid ? Pending : Current;
        if (FrameReceived != Invalid && Received[FrameReceived].Sequence <= Displayed) FrameReceived = Invalid;
        if (FrameReceived != Invalid) Received[FrameReceived].Pinned = true;
    }
};

ParticleTransfer::ParticleTransfer(std::shared_ptr<SimulationDomain> domain, GDX12DeviceResources& primary)
    : _impl(std::make_unique<Impl>(std::move(domain), primary)) {}
ParticleTransfer::~ParticleTransfer() {
    try { Stop(); } catch (...) {
        _impl->Cancel.store(true); _impl->Wake.notify_all();
        if (_impl->Worker.joinable()) _impl->Worker.join();
        OutputDebugStringA("Meta particle drain failed; retaining complete domain\n");
        _impl.release();
    }
}
void ParticleTransfer::Stop() { _impl->CheckOwner(); _impl->Stop(); }
const std::vector<uint64_t>& ParticleTransfer::Emitters() const { return _impl->Ids; }
GDX12Resource* ParticleTransfer::Display() const { return _impl->Display ? &_impl->Display->GetResource() : nullptr; }

void ParticleTransfer::Update(GDX12ParticleSimulation& primary, GDX12ParticleInteractionField field,
                             float clockScale, bool reset) {
    auto& p = *_impl; p.CheckOwner();
    std::vector<GDX12ParticleEmitter> emitters;
    if (primary._state && primary._state->Active)
        for (const auto& e : primary._state->Emitters) if (CanOffloadParticleEmitter(e)) emitters.push_back(e);
    std::sort(emitters.begin(), emitters.end(), [](const auto& a, const auto& b) { return a.Id < b.Id; });
    const bool interaction = std::any_of(emitters.begin(), emitters.end(), [](const auto& e) { return e.InteractionEnabled; });
    if (interaction && (!field.Resource || !field.Resolution || field.Resolution > 4096))
        throw std::invalid_argument("secondary particles require their real interaction field");
    const UINT resolution = interaction ? field.Resolution : 0;
    if (resolution && field.Resource->D3DResource->GetDesc().Width < uint64_t(resolution) * resolution * sizeof(XMFLOAT4))
        throw std::invalid_argument("particle interaction field is too small");
    bool changed = reset || resolution != p.Resolution || emitters.size() != p.Entries.size();
    for (size_t i = 0; !changed && i < emitters.size(); ++i)
        changed = emitters[i].Id != p.Entries[i].Id || emitters[i].ResetRevision != p.Entries[i].ResetRevision ||
            emitters[i].MaxParticles != p.Entries[i].Capacity;
    if (changed) {
        p.Stop();
        std::unordered_map<uint64_t, uint64_t> consumed;
        p.ReturningBursts.clear();
        if (primary._state && p.Simulation._state)
            for (const auto& e : primary._state->Emitters)
                if (primary._state->ExternalOffsets.count(e.Id) && !CanOffloadParticleEmitter(e)) {
                    const auto old = p.Simulation._state->Pools.find(e.Id);
                    if (!reset && old != p.Simulation._state->Pools.end() && old->second->ResetRevision == e.ResetRevision)
                        p.ReturningBursts[e.Id] = old->second->BurstTotal;
                }
        for (const auto& e : emitters) {
            if (reset) continue;
            const auto* state = p.Simulation._state.get();
            if (state) {
                const auto old = state->Pools.find(e.Id);
                if (old != state->Pools.end() && old->second->ResetRevision == e.ResetRevision) {
                    consumed[e.Id] = old->second->BurstTotal; continue;
                }
            }
            if (primary._state) {
                const auto old = primary._state->Pools.find(e.Id);
                if (old != primary._state->Pools.end() && old->second->ResetRevision == e.ResetRevision)
                    consumed[e.Id] = old->second->BurstTotal;
            }
        }
        p.Configure(emitters, resolution, consumed, reset);
    }
    {
        std::lock_guard<std::mutex> lock(p.Mutex);
        if (p.Failure) std::rethrow_exception(p.Failure);
        p.Inputs = std::move(emitters); p.HostField = field; p.ClockScale = clockScale;
        p.ResetMotion = reset || (primary._state && primary._state->ResetMotion);
    }
    p.Start(); p.Wake.notify_all();
}
void ParticleTransfer::Bind(GDX12ParticleSimulation& simulation) {
    if (!simulation._state) return;
    auto& state = *simulation._state; auto& p = *_impl;
    std::unordered_map<uint64_t, UINT> offsets;
    for (const auto& entry : p.Entries) offsets.emplace(entry.Id, entry.Base);
    // Placement changes reset the moved pool while retaining consumed burst identity.
    for (const auto& emitter : state.Emitters)
        if ((state.ExternalOffsets.count(emitter.Id) != 0) != (offsets.count(emitter.Id) != 0)) {
            auto& pool = *state.Pools.at(emitter.Id);
            pool.Initialized = false;
            if (const auto burst = p.ReturningBursts.find(emitter.Id); burst != p.ReturningBursts.end())
                pool.BurstTotal = burst->second;
            state.ResetMotion = true;
        }
    p.ReturningBursts.clear();
    state.ExternalOffsets = std::move(offsets);
    state.ExternalDisplay = Display();
    state.ExternalSRV = p.Display ? p.Display->GetSRV()->GPUHandle : D3D12_GPU_DESCRIPTOR_HANDLE{};
}
void ParticleTransfer::Append(Graph& graph, std::vector<ID3D12Resource*>& bindings, PublicationSelection selection) {
    auto& p = *_impl; p.CheckOwner();
    if (selection != PublicationSelection::StrictPublication && selection != PublicationSelection::LatestCompleted)
        throw std::invalid_argument("unknown particle publication selection");
    if (graph.Device != 0 || graph.Resources.size() != bindings.size() || p.HistoryOp != Invalid || p.InputOp != Invalid)
        throw std::logic_error("invalid particle graph lifecycle");
    std::lock_guard<std::mutex> lock(p.Mutex);
    if (p.Failure) std::rethrow_exception(p.Failure);
    if (!p.Display) return;
    p.Select(selection);
    const Id display = NativeWork::ResourceFor(graph, bindings, p.Display->GetResource().D3DResource.Get(), "particle displayed batch");
    std::vector<Access> history{{display, Mode::ReadWrite, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE}};
    if (p.FrameReceived != Invalid) {
        const Id received = NativeWork::ResourceFor(graph, bindings,
            p.Received[p.FrameReceived].Data->GetResource().D3DResource.Get(), "particle received payload");
        history.push_back({received, Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE});
    }
    p.HistoryOp = Id(graph.Operations.size());
    graph.Operations.push_back({"particle displayed identity/history", history, {}, false, 1, true, Queue::Direct});
    if (!p.Resolution) return;
    for (Id i = 0; i < Slots; ++i) {
        auto& input = p.Input[i];
        if (input.Pinned || (input.Claimed && (!input.Consumed || !p.Submit->Complete(input.Consumed)))) continue;
        if (input.Ready && !p.Submit->Complete(input.Ready)) continue;
        p.FrameInput = i; input.Pinned = true; input.Claimed = false; input.Consumed = {};
        const Id from = NativeWork::ResourceFor(graph, bindings, p.HostField.Resource->D3DResource.Get(), "particle scene interaction field");
        const Id to = Id(graph.Resources.size());
        graph.Resources.push_back({"particle primary shared interaction", 0, 0, false, true});
        bindings.push_back(input.Primary.Get());
        p.InputOp = Id(graph.Operations.size());
        graph.Operations.push_back({"particle common interaction COPY export", {{from, Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE},
            {to, Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST}}, {}, false, 1, false, Queue::Copy});
        input.Field = p.HostField;
        break;
    }
}
bool ParticleTransfer::Record(Id op, ID3D12GraphicsCommandList* cmd) {
    auto& p = *_impl;
    if (op == Invalid) return false;
    if (op == p.InputOp) {
        cmd->CopyBufferRegion(p.Input[p.FrameInput].Primary.Get(), 0, p.HostField.Resource->D3DResource.Get(), 0,
            uint64_t(p.Resolution) * p.Resolution * sizeof(XMFLOAT4)); return true;
    }
    if (op != p.HistoryOp) return false;
    std::vector<GDX12RecordingStates::Binding> states{
        {p.Display->GetResource().D3DResource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE}};
    if (p.FrameReceived != Invalid) states.emplace_back(
        p.Received[p.FrameReceived].Data->GetResource().D3DResource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    GDX12RecordingStates tracker(states);
    GDX12CommandList borrowed(cmd); borrowed.SetDescriptorHeaps({p.Primary.SRV_UAV_Heap.get()});
    borrowed.SetComputeRootSignature(p.DisplayRoot.get());
    borrowed.SetPipelineState(p.FrameReceived == Invalid ? p.RepeatPSO : p.DisplayPSO);
    ParticleResources::UAV(&borrowed, *p.Display);
    if (p.FrameReceived != Invalid) borrowed.SetComputeSRV(2, p.Received[p.FrameReceived].Data->GetSRV()->GPUHandle);
    borrowed.SetComputeUAV(1, p.Display->GetUAV()->GPUHandle);
    for (size_t i = 0; i < p.Entries.size(); ++i) {
        auto parameters = p.Parameters(i, p.FrameReceived == Invalid ? p.Displayed : p.Received[p.FrameReceived].Sequence);
        parameters[14] = p.FrameReceived != Invalid;
        parameters[15] = p.SeedDisplay ? 1u : p.ResetMotion ? 2u : 0u;
        cmd->SetComputeRoot32BitConstants(3, 16, parameters.data(), 0);
        borrowed.Dispatch((p.Entries[i].Capacity + 255) / 256, 1, 1);
    }
    borrowed.ResourceBarrier({p.Display->GetResource().GetUAVBarrier()}); tracker.Finish(cmd); return true;
}
std::vector<PacketWait> ParticleTransfer::Waits(const Schedule& schedule) const {
    const auto& p = *_impl;
    if (p.HistoryOp == Invalid || p.FrameReceived == Invalid) return {};
    return {{PacketFor(schedule, p.HistoryOp), p.Received[p.FrameReceived].Imported}};
}
void ParticleTransfer::RetireFrame(const Schedule& schedule, const std::vector<CompletionToken>& tokens, bool success) {
    auto& p = *_impl; p.CheckOwner();
    if (tokens.size() != schedule.Packets.size()) throw std::logic_error("particle retirement evidence missing");
    std::lock_guard<std::mutex> lock(p.Mutex);
    for (auto token : tokens) if (token) {
        if (token.Device != 0) throw std::logic_error("foreign particle reader token");
        p.Submit->Complete(token);
        const auto q = token.Target == Queue::Direct ? 0 : token.Target == Queue::Compute ? 1 : 2;
        if (token.Value > p.FrameTails[q].Value) p.FrameTails[q] = token;
    }
    if (p.HistoryOp != Invalid) {
        const auto token = tokens.at(PacketFor(schedule, p.HistoryOp));
        if (success && !token) throw std::logic_error("particle history not submitted");
        if (p.FrameReceived != Invalid) {
            auto& r = p.Received[p.FrameReceived]; r.History = token; r.Pinned = false;
            if (success) p.Displayed = r.Sequence;
        }
        if (success) p.SeedDisplay = false;
    }
    if (p.InputOp != Invalid) {
        const auto token = tokens.at(PacketFor(schedule, p.InputOp));
        auto& input = p.Input[p.FrameInput];
        input.Pinned = false;
        if (success) {
            if (!token || p.InputSequence == UINT64_MAX) throw std::logic_error("particle input not submitted");
            input.Ready = token; input.Sequence = ++p.InputSequence;
        } else { input.Ready = {}; input.Sequence = 0; }
    }
    p.FrameInput = p.FrameReceived = p.InputOp = p.HistoryOp = Invalid; p.Wake.notify_all();
}
}

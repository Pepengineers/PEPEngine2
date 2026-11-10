#include "Rendering/Water/CrossAdapterWater.h"
#include "Rendering/Water/GDX12FluidClock.h"
#include "App.Base/Rendering/RenderMGPURuntime.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12CommandQueue.h"
#include "Engine.RendererDX12/GDX12SnapshotTransport.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <functional>
#include <stdexcept>
#include <utility>

namespace
{
    using namespace GDX12Snapshot;
    using Graph = GDX12RenderGraph;
    using Access = Graph::Access;
    using Mode = Graph::Mode;
    using Clock = GDX12FluidClock<GDX12FluidProducerInput>;
    using HostClock = std::chrono::steady_clock;
    constexpr auto UAV = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    constexpr auto CopySource = D3D12_RESOURCE_STATE_COPY_SOURCE;
    constexpr auto CopyDest = D3D12_RESOURCE_STATE_COPY_DEST;

    void Require(bool condition, const char* message)
    {
        if (!condition) throw std::logic_error(message);
    }

    class WaterBatch final : public GDX12RenderPass
    {
    public:
        using Recorder = std::function<void(GDX12CommandList&, const Graph::BoundResources&)>;
        WaterBatch(const char* name, Placement placement, std::vector<Access> uses, Recorder record)
            : _name(name), _placement(placement), _uses(std::move(uses)), _record(std::move(record)) {}
        void RecordRenderGraph(GDX12RenderGraphBuilder& graph) override
        {
            graph.SetName(_name);
            graph.SetPlacement(_placement);
            graph.DeclareCompleteAccesses();
            graph.HasSideEffects();
            for (const auto& use : _uses)
            {
                if (use.Version.IsValid()) graph.UseVersion(use.Id, use.Usage, use.Version, use.State);
                else graph.Use(use.Id, use.Usage, use.State);
            }
        }
        bool SupportsGraphQueue(QueueKind queue) const override { return _placement.Queue == queue; }
        void RecordPlacedCommands(GDX12CommandList* list, const Graph::BoundResources& views) override
        { _record(*list, views); }
    private:
        const char* _name;
        Placement _placement;
        std::vector<Access> _uses;
        Recorder _record;
    };

    Graph::Resource Bind(Graph& graph, Graph::ExecutionBindings& bindings, const char* name,
        const PhysicalView& view, bool imported, bool shared = false)
    {
        const auto key = imported ? graph.Import(name) : graph.Create(name);
        graph.RegisterResource(key, view.Identity, shared);
        bindings.Views.push_back({key, view.Identity, view.Resource});
        return key;
    }

    CompletionPoint ReceiptPoint(const Graph& graph, const Graph::ExecutionReceipt& receipt, Graph::Pass pass)
    {
        for (const auto& batch : graph.GetCompiledPlan().Batches)
            if (std::find(batch.Passes.begin(), batch.Passes.end(), pass) != batch.Passes.end())
                return receipt.Batches.at(static_cast<std::size_t>(batch.Id));
        throw std::logic_error("Water pass is missing its batch receipt");
    }

    bool Reached(GDX12CommandQueue& queue, CompletionPoint point)
    {
        const auto progress = queue.GetCompletionProgress(point.Timeline());
        if (progress.CompletedValue == (std::numeric_limits<std::uint64_t>::max)())
            ThrowIfFailed(DXGI_ERROR_DEVICE_REMOVED);
        return point.IsReachedBy(progress);
    }

    struct SolverSlot
    {
        std::unique_ptr<GDX12DescriptorHeap> Heap;
        GDX12FluidSolver Solver;
        GDX12FluidSolver::Buffers Buffers;
        std::unique_ptr<GDX12UploadBuffer<GDX12FluidParameters>> Constants;
    };

    struct Metadata
    {
        TransferVersion Version;
        GDX12FluidProducerInput Input;
        double Time = 0;
        HostClock::time_point Captured;
        bool Displayed = false;
    };

    struct Output
    {
        Reservation Credit;
        Metadata Snapshot;
        std::optional<CompletionPoint> ExportInput;
        ImportPlan Import;
        bool Exported = false;
    };

    struct Solve
    {
        Clock::Batch Ticks;
        GDX12FluidProducerInput Seed;
        TransferVersion Version;
        Reservation Credit;
        HostClock::time_point Captured;
        bool Publishing = false;
        bool Exact = false;
        bool TransportPrepared = false;
        std::optional<PreparedRead> HandoffReader;
        std::size_t Count() const { return Ticks.Size() ? Ticks.Size() : 1; }
        const GDX12FluidProducerInput& Input(std::size_t i) const { return Ticks.Size() ? Ticks[i].Parameters : Seed; }
        float Seconds(std::size_t i) const { return Ticks.Size() ? static_cast<float>(Ticks[i].Seconds) : 0; }
    };

    struct DisplayRead
    {
        ReadLease Read;
        Matrix Transform;
    };

    struct FrameUse
    {
        DisplayRead Current;
        std::optional<DisplayRead> Previous;
        DisplayRead NextHistory;
        CompletionPoint Tail;
    };
}

struct CrossAdapterWater::State
{
    RenderMGPURuntime* Runtime = nullptr;
    GDX12DeviceResources* Primary = nullptr;
    GDX12Device* Secondary = nullptr;
    std::shared_ptr<Transport> Bank;
    TransferId Stream{};
    Epoch CurrentEpoch{};
    std::uint64_t Invocation = 0;
    std::uint64_t Owner = 0, ResetRevision = 0;
    std::shared_ptr<const std::vector<GDX12FluidParticle>> Seed;
    bool SecondaryOwns = false, Exact = false, LayoutReady = false, BankNeedsResize = false;
    bool Enabled = false, Attempt = false, Visible = false, Reference = false;
    bool RequestedRoute = false, RequestedExact = false, HistoryConsecutive = false, DemandExact = false;
    bool PublishCurrent = false;
    std::optional<GDX12FluidProducerInput> Input, EpochSeed, HeadInput;
    Matrix DisplayTransform = Matrix::Identity;
    HostClock::time_point LastProducer;
    float MaxAgeMs = 0;
    std::optional<Clock> Time;
    std::shared_ptr<SolverSlot> Slot;
    GDX12FluidSolver::Head Head;
    std::array<PhysicalView, 7> SolverViews;
    PhysicalView Source;
    Ticket SourceTicket;
    CompletionPoint SolverDone;
    Clock::Batch IssuedTicks;
    Epoch IssuedEpoch{};
    std::optional<Solve> Prepared;
    std::array<std::optional<Output>, SlotCount> Outputs;
    std::array<std::optional<Metadata>, SlotCount> Local;
    std::array<std::unique_ptr<GDX12Descriptor>, SlotCount> SRVs;
    std::array<std::optional<FrameUse>, SlotCount> Frames;
    std::optional<DisplayRead> History;
    std::optional<std::size_t> AwaitingTail;
    std::optional<GDX12FluidDisplay> Selected;
    Diagnostics Counters;

    GDX12CommandQueue& Compute() const
    { return *Runtime->GetQueue({Secondary->GetSubmissionAdapterId(), QueueKind::Compute}); }
    GDX12CommandQueue& Direct() const { return *Primary->Device->GetCommandQueue(); }
    Placement SecondaryLane(QueueKind queue) const { return {Secondary->GetSubmissionAdapterId(), queue}; }
    Placement PrimaryLane(QueueKind queue) const { return {Primary->Device->GetSubmissionAdapterId(), queue}; }

    void Collect();
    void ClearHistory();
    void CancelSolve();
    void CancelPending();
    void BeginEpoch(const GDX12FluidInstance* instance, const GDX12FluidControls& controls,
        RenderMGPURuntime* runtime, GDX12DeviceResources& primary, GDX12Device* secondary);
    bool Configure();
    TransferVersion NewVersion();
    bool PrepareSolve(bool exact);
    std::optional<CompletionPoint> SubmitSolve(bool exact);
    void AdvanceOutputs();
    bool TryExport(Output& output);
    bool TryImport(Output& output);
    std::optional<std::size_t> FreeFrame() const;
    bool Select(Ticket ticket, Metadata& metadata);
};

CrossAdapterWater::CrossAdapterWater() : _state(std::make_unique<State>()) {}
CrossAdapterWater::~CrossAdapterWater() = default;

void CrossAdapterWater::State::Collect()
{
    if (SolverDone.IsValid() && Reached(Compute(), SolverDone))
    {
        if (IssuedEpoch == CurrentEpoch && IssuedTicks.Size())
            Require(Time->Complete(IssuedTicks), "Water solver completion did not match its clock");
        SolverDone = {};
        IssuedTicks = {};
    }
    for (auto& frame : Frames)
        if (frame && frame->Tail.IsValid() && Reached(Direct(), frame->Tail)) frame.reset();
}

void CrossAdapterWater::State::ClearHistory()
{
    if (History) Require(Bank->CancelReader(History->Read.Lease.Id), "Water history lease could not retire");
    History.reset();
    HistoryConsecutive = false;
}

void CrossAdapterWater::State::CancelSolve()
{
    if (Prepared && Prepared->Version.IsValid())
    {
        if (Prepared->Credit.Status == Result::Accepted)
            Require(Bank->Cancel(Prepared->Credit.Publication), "Water prepared solver could not cancel");
        else if (Prepared->Exact) Bank->CancelDeferredExact(Prepared->Version);
    }
    Prepared.reset();
}

void CrossAdapterWater::State::CancelPending()
{
    CancelSolve();
    for (auto& output : Outputs)
    {
        if (output)
            Require(Bank->Cancel(output->Credit.Publication), "Water output could not cancel its unsubmitted stage");
        output.reset();
    }
}

void CrossAdapterWater::State::BeginEpoch(const GDX12FluidInstance* instance, const GDX12FluidControls& controls,
    RenderMGPURuntime* runtime, GDX12DeviceResources& primary, GDX12Device* secondary)
{
    if (Bank)
    {
        Require(Runtime == runtime, "Water runtime changed before its bank retired");
        ClearHistory();
        CancelPending();
        CurrentEpoch = Runtime->NewEpoch();
        Require(Bank->Reset(CurrentEpoch), "Water reset found an unresolved transport transaction");
        BankNeedsResize = true;
        Counters.ResetSuperseded += Time->Reset(static_cast<std::uint64_t>(CurrentEpoch));
    }
    Selected.reset();
    Local = {};
    Owner = instance ? instance->Id : 0;
    ResetRevision = instance ? instance->Component.GetResetRevision() : 0;
    Seed = instance ? instance->Component.GetParticles() : nullptr;
    Reference = instance && instance->Component.GetSettings().MGPUReference;
    RequestedRoute = controls.MultiGPU;
    RequestedExact = controls.MultiGPUExact;
    SecondaryOwns = Owner && Seed && !Seed->empty() && controls.MultiGPU && runtime && secondary &&
        primary.Device && primary.Device != secondary &&
        runtime->GetQueue({secondary->GetSubmissionAdapterId(), QueueKind::Compute});
    Exact = SecondaryOwns && controls.MultiGPUExact;
    LayoutReady = false;
    Invocation = 0;
    LastProducer = {};
    Head = {};
    PublishCurrent = true;
    HeadInput.reset();
    EpochSeed.reset();
    if (!SecondaryOwns)
    {
        Counters.Status = controls.MultiGPU && Owner ? "startup-unavailable" : "local";
        return;
    }
    Runtime = runtime;
    Primary = &primary;
    Secondary = secondary;
    EpochSeed.emplace(*instance, controls.Quality);
    if (!Bank)
    {
        if (Stream == TransferId::Invalid) Stream = Runtime->NewTransferId();
        CurrentEpoch = Runtime->NewEpoch();
        Time.emplace(static_cast<std::uint64_t>(CurrentEpoch));
    }
}

bool CrossAdapterWater::State::Configure()
{
    if (LayoutReady) return true;
    if (SolverDone.IsValid() || std::any_of(Frames.begin(), Frames.end(), [](const auto& frame) { return bool(frame); }))
        return false;
    const auto desc = CD3DX12_RESOURCE_DESC::Buffer(static_cast<UINT64>(Seed->size()) * sizeof(GDX12FluidSolver::Particle));
    if (!Bank)
    {
        try
        {
            Bank = Runtime->CreateBank(Stream, CurrentEpoch, SecondaryLane(QueueKind::Compute).Adapter,
                PrimaryLane(QueueKind::Direct).Adapter, desc);
        }
        catch (const DxException& error)
        {
            if (error.ErrorCode != DXGI_ERROR_UNSUPPORTED && error.ErrorCode != E_NOTIMPL) throw;
            SecondaryOwns = Exact = false;
            Counters.Status = "startup-unavailable";
            Counters.ResetRequired = true;
            return false;
        }
    }
    else if (BankNeedsResize)
    {
        SRVs = {};
        if (!Runtime->TryReconfigureBank(*Bank, desc)) return false;
        BankNeedsResize = false;
    }

    auto slot = std::make_shared<SolverSlot>();
    slot->Heap = std::make_unique<GDX12DescriptorHeap>(Secondary, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        TextureResources_StartIndex + TextureResources_RangeLength, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
    slot->Solver.Initialize(Secondary);
    slot->Buffers = GDX12FluidSolver::CreateBuffers(Secondary, slot->Heap.get(), EpochSeed->Instance());
    slot->Constants = std::make_unique<GDX12UploadBuffer<GDX12FluidParameters>>(Secondary,
        static_cast<UINT>(Clock::TicksPerInvocation));
    const auto identity = [&](TransferId stream)
    {
        return ResourceIdentity{Runtime->NewResourceId(), SecondaryLane(QueueKind::Compute).Adapter,
            CurrentEpoch, stream, Runtime->NewAllocationId()};
    };
    GDX12Resource* resources[] = {
        &slot->Buffers.Particles[0]->GetResource(), &slot->Buffers.Particles[1]->GetResource(),
        &slot->Buffers.Density->GetResource(), &slot->Buffers.Keys->GetResource(), &slot->Buffers.Offsets->GetResource(),
        &slot->Buffers.SeedUpload->GetResource(), &slot->Constants->GetResource()
    };
    for (std::size_t i = 0; i < SolverViews.size(); ++i) SolverViews[i] = {{slot, resources[i]}, identity({})};
    auto source = std::make_shared<GDX12Resource>();
    source->D3DResource = Secondary->CreateResource(D3D12_HEAP_TYPE_DEFAULT, desc, D3D12_RESOURCE_STATE_COMMON);
    Source = {std::move(source), identity(Stream)};
    SourceTicket = {};
    Slot = std::move(slot);

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_UNKNOWN;
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.NumElements = Slot->Buffers.Count;
    srv.Buffer.StructureByteStride = sizeof(GDX12FluidSolver::Particle);
    auto& heap = *Primary->SRV_UAV_Heap;
    for (std::size_t i = 0; i < SRVs.size(); ++i)
    {
        auto descriptor = std::make_unique<GDX12Descriptor>();
        descriptor->InitAsSRV(Bank->GetSlots()[i].Local.Resource->D3DResource.Get(), &srv, &heap,
            heap.GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength));
        SRVs[i] = std::move(descriptor);
    }
    LayoutReady = true;
    return true;
}

TransferVersion CrossAdapterWater::State::NewVersion()
{
    if (Invocation == (std::numeric_limits<std::uint64_t>::max)()) throw std::overflow_error("Water versions exhausted");
    return {Stream, CurrentEpoch, ProducerInvocationId{++Invocation}};
}

bool CrossAdapterWater::State::PrepareSolve(bool exact)
{
    if (SolverDone.IsValid() || !Enabled) return false;
    if (!Prepared)
    {
        if (!Attempt) return false;
        const auto batch = Time->Prepare();
        const bool publishing = Visible && !Time->GetProgress().FirstMissingInput;
        if (!batch.Size() && !Head.SeedPending && !(publishing && PublishCurrent)) return false;
        Prepared.emplace(Solve{batch, *Input, publishing ? NewVersion() : TransferVersion{},
            {}, HostClock::now(), publishing, exact && publishing});
    }
    return Prepared->Exact == exact;
}

std::optional<CompletionPoint> CrossAdapterWater::State::SubmitSolve(bool exact)
{
    if (!PrepareSolve(exact)) return {};
    auto& solve = *Prepared;
    const bool simulate = Head.SeedPending || solve.Ticks.Size();
    if (solve.Publishing && solve.Credit.Status != Result::Accepted)
    {
        if ((!exact || FreeFrame()) && (!SourceTicket.Id() || Bank->SourceReusable(SourceTicket)))
        {
            solve.Credit = Bank->Reserve(solve.Version,
                exact ? SnapshotReadMode::ExactVersion : SnapshotReadMode::LatestCompleted,
                Compute(), Source.Resource->D3DResource, Source.Identity.Allocation);
            Require(solve.Credit.Status == Result::Accepted || solve.Credit.Status == Result::Deferred,
                "Water transport rejected its publication identity");
        }
        if (solve.Credit.Status != Result::Accepted)
        {
            if (exact || !simulate) return {};
            solve.Publishing = false;
        }
    }
    const auto ticket = solve.Credit.Publication;
    if (exact && !solve.TransportPrepared)
    {
        Require(Bank->PrepareExact(ticket), "Water Exact copy preparation failed");
        solve.TransportPrepared = true;
        solve.HandoffReader = Bank->PrepareExactReader(ticket, Direct());
        Require(solve.HandoffReader.has_value(), "Water Exact handoff reader capacity exhausted");
    }

    Graph graph;
    Graph::ExecutionBindings bindings;
    bindings.KeepAlive = {Slot, Bank};
    constexpr const char* names[] = {"Water particles A", "Water particles B", "Water density",
        "Water keys", "Water offsets", "Water seed", "Water solver constants"};
    std::array<Graph::Resource, 7> views{};
    std::vector<Access> uses;
    for (std::size_t i = 0; simulate && i < 5; ++i)
    {
        views[i] = Bind(graph, bindings, names[i], SolverViews[i], true);
        uses.push_back({views[i], Mode::ReadWrite, UAV});
    }
    if (simulate)
    {
        if (Head.SeedPending)
            uses.push_back({Bind(graph, bindings, names[5], SolverViews[5], true), Mode::Read, CopySource});
        uses.push_back({Bind(graph, bindings, names[6], SolverViews[6], true), Mode::Read,
            D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER});
    }

    std::array<GDX12FluidSolver::Steps, Clock::TicksPerInvocation> steps{};
    UINT final = Head.Current;
    for (std::size_t i = 0; simulate && i < solve.Count(); ++i)
    {
        const auto& input = solve.Input(i);
        steps[i] = Slot->Solver.PlanSteps(input.Settings(), solve.Seconds(i));
        final ^= steps[i].Count & 1;
        Slot->Constants->CopyData(static_cast<UINT>(i),
            MakeFluidParameters(input.Instance().ContainerToWorld, input.Settings()));
    }
    auto staged = Head;
    WaterBatch simulation("Water solve", SecondaryLane(QueueKind::Compute), std::move(uses),
        [&](GDX12CommandList& commands, const Graph::BoundResources&)
        {
            commands.SetDescriptorHeaps({Slot->Heap.get()});
            for (std::size_t i = 0; i < solve.Count(); ++i)
                staged = Slot->Solver.Record(commands, Slot->Buffers, staged, solve.Input(i).Settings(),
                    steps[i], Slot->Constants->GetElementAddress(static_cast<UINT>(i)),
                    GDX12FluidSolver::Boundary::PlacedUav);
            Require(staged.Current == final, "Water solver particle head differs from its declared output");
        });
    const auto simulationPass = simulate ? graph.AddPass(simulation) : Graph::Pass{};

    std::optional<WaterBatch> pack, exportCopy, importCopy, handoff;
    auto producerPass = simulationPass;
    Graph::Pass exportPass{}, importPass{}, handoffPass{};
    if (solve.Publishing)
    {
        const auto particles = simulate ? views[final] : Bind(graph, bindings, names[final], SolverViews[final], true);
        const auto source = Bind(graph, bindings, "Water publication source", Source, false);
        pack.emplace("Water pack", SecondaryLane(QueueKind::Compute), std::vector<Access>{
            {particles, Mode::Read, CopySource}, {source, Mode::Write, CopyDest, solve.Version}},
            [&, particles, source](GDX12CommandList& commands, const Graph::BoundResources& bound)
            { Slot->Solver.RecordPublish(commands, *bound.Get(particles), *bound.Get(source), Slot->Buffers.Count); });
        producerPass = graph.AddPass(*pack);
        if (simulate) graph.DependsOn(producerPass, simulationPass);
        if (exact)
        {
            const auto& shared = Bank->GetSlots()[solve.Credit.SharedSlot];
            const auto out = Bind(graph, bindings, "Water shared export", shared.Export, false, true);
            const auto in = Bind(graph, bindings, "Water shared import", shared.Import, false, true);
            const auto local = Bind(graph, bindings, "Water primary particles", solve.HandoffReader->View, false);
            exportCopy.emplace("Water Exact export", SecondaryLane(QueueKind::Copy), std::vector<Access>{
                {source, Mode::Read, CopySource, solve.Version}, {out, Mode::Write, CopyDest, solve.Version}},
                [&](GDX12CommandList& commands, const Graph::BoundResources&)
                { Bank->RecordExportCopies(ticket, *commands.GetCommandList().Get()); });
            importCopy.emplace("Water Exact import", PrimaryLane(QueueKind::Copy), std::vector<Access>{
                {in, Mode::Read, CopySource, solve.Version}, {local, Mode::Write, CopyDest, solve.Version}},
                [&](GDX12CommandList& commands, const Graph::BoundResources&)
                { Bank->RecordImportCopies(ticket, *commands.GetCommandList().Get()); });
            handoff.emplace("Water Exact surface handoff", PrimaryLane(QueueKind::Direct), std::vector<Access>{
                {local, Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, solve.Version}},
                [&, local](GDX12CommandList&, const Graph::BoundResources& bound)
                {
                    Require(bound.Get(local) == solve.HandoffReader->View.Resource->D3DResource.Get(),
                        "Water handoff has a different primary view");
                });
            exportPass = graph.AddPass(*exportCopy);
            importPass = graph.AddPass(*importCopy);
            handoffPass = graph.AddPass(*handoff);
            graph.DependsOn(importPass, exportPass);
        }
    }
    graph.CompilePlan();
    std::optional<CompletionPoint> boundary;
    const auto receipt = Runtime->TrySubmit(graph, std::move(bindings), [&](const Graph::ExecutionReceipt& issued)
    {
        const auto produced = ReceiptPoint(graph, issued, producerPass);
        if (solve.Publishing)
        {
            const Metadata metadata{solve.Version, simulate ? solve.Input(solve.Count() - 1) : *HeadInput,
                solve.Ticks.EndTime(), solve.Captured};
            if (exact)
            {
                const auto exported = ReceiptPoint(graph, issued, exportPass);
                const auto imported = ReceiptPoint(graph, issued, importPass);
                boundary = ReceiptPoint(graph, issued, handoffPass);
                Bank->CommitExact(ticket, produced, exported, exported, imported, imported);
                Local[solve.Credit.LocalSlot] = metadata;
                Require(Bank->BindReader(solve.HandoffReader->Id, *boundary), "Water Exact handoff receipt rejected");
                Require(Select(ticket, *Local[solve.Credit.LocalSlot]), "Water Exact display admission failed after submit");
                Require(Bank->ReleaseExact(ticket), "Water Exact pin could not release");
            }
            else
            {
                Require(Bank->BindProducer(ticket, produced), "Water Latest solver receipt rejected");
                auto& output = Outputs[solve.Credit.SharedSlot];
                Require(!output, "Water shared slot was reused before import submission");
                output.emplace(Output{solve.Credit, metadata});
            }
            SourceTicket = ticket;
            PublishCurrent = false;
        }
        else if (solve.Version.IsValid()) ++Counters.SkippedPublications;
        if (solve.Ticks.Size()) Require(Time->Commit(solve.Ticks), "Water solver receipt did not match its clock");
        Head = staged;
        SolverDone = produced;
        IssuedTicks = solve.Ticks;
        IssuedEpoch = CurrentEpoch;
        LastProducer = HostClock::now();
        if (simulate)
        {
            HeadInput = solve.Input(solve.Count() - 1);
            ++Counters.SolverInvocations;
        }
    });
    if (receipt) Prepared.reset();
    return boundary;
}

bool CrossAdapterWater::State::TryExport(Output& output)
{
    const auto ticket = output.Credit.Publication;
    if (!output.ExportInput) output.ExportInput = Bank->PrepareExport(ticket);
    Require(output.ExportInput.has_value(), "Water Latest export has no issued producer");
    Graph graph;
    Graph::ExecutionBindings bindings;
    const auto version = output.Snapshot.Version;
    const auto source = Bind(graph, bindings, "Water completed producer", Source, true);
    const auto destination = Bind(graph, bindings, "Water shared export",
        Bank->GetSlots()[output.Credit.SharedSlot].Export, false, true);
    graph.ImportVersion(source, version);
    bindings.ExactInputs.push_back({source, version, *output.ExportInput, Bank});
    WaterBatch copy("Water Latest export", SecondaryLane(QueueKind::Copy),
        {{source, Mode::Read, CopySource, version}, {destination, Mode::Write, CopyDest, version}},
        [&](GDX12CommandList& commands, const Graph::BoundResources&)
        { Bank->RecordExportCopies(ticket, *commands.GetCommandList().Get()); });
    graph.AddPass(copy);
    graph.CompilePlan();
    return Runtime->TrySubmit(graph, std::move(bindings), [&](const Graph::ExecutionReceipt& issued)
    {
        const auto point = issued.Batches.front();
        Bank->CommitExport(ticket, point, point);
    }).has_value();
}

bool CrossAdapterWater::State::TryImport(Output& output)
{
    const auto ticket = output.Credit.Publication;
    if (output.Import.Status != Result::Accepted) output.Import = Bank->PrepareImport(ticket);
    if (output.Import.Status != Result::Accepted) return false;
    const auto& shared = Bank->GetSlots()[output.Credit.SharedSlot];
    const auto local = output.Import.LocalSlot;
    const auto version = output.Snapshot.Version;
    Graph graph;
    Graph::ExecutionBindings bindings;
    const auto source = Bind(graph, bindings, "Water shared import", shared.Import, true, true);
    const auto destination = Bind(graph, bindings, "Water primary particles", Bank->GetSlots()[local].Local, false);
    graph.ImportVersion(source, version);
    bindings.ExactInputs.push_back({source, version, output.Import.ExportReady, Bank,
        Runtime->GetQueue(SecondaryLane(QueueKind::Copy)), shared.Export.Identity});
    WaterBatch copy("Water Latest import", PrimaryLane(QueueKind::Copy),
        {{source, Mode::Read, CopySource, version}, {destination, Mode::Write, CopyDest, version}},
        [&](GDX12CommandList& commands, const Graph::BoundResources&)
        { Bank->RecordImportCopies(ticket, *commands.GetCommandList().Get()); });
    graph.AddPass(copy);
    graph.CompilePlan();
    return Runtime->TrySubmit(graph, std::move(bindings), [&](const Graph::ExecutionReceipt& issued)
    {
        const auto point = issued.Batches.front();
        Bank->CommitImport(ticket, point, point);
        if (Local[local] && !Local[local]->Displayed) ++Counters.DiscardedReady;
        Local[local] = output.Snapshot;
    }).has_value();
}

void CrossAdapterWater::State::AdvanceOutputs()
{
    for (auto& output : Outputs)
    {
        if (!output) continue;
        if (!output->Exported) output->Exported = TryExport(*output);
        if (output->Exported && TryImport(*output)) output.reset();
    }
}

std::optional<std::size_t> CrossAdapterWater::State::FreeFrame() const
{
    for (std::size_t i = 0; i < Frames.size(); ++i)
        if (!Frames[i]) return i;
    return {};
}

bool CrossAdapterWater::State::Select(Ticket ticket, Metadata& metadata)
{
    const auto frame = FreeFrame();
    if (!frame || AwaitingTail || metadata.Version.VersionEpoch != CurrentEpoch) return false;
    auto current = Bank->AcquireReader(ticket, Direct());
    if (!current) return false;
    auto next = Bank->AcquireReader(ticket, Direct());
    if (!next)
    {
        Require(Bank->CancelReader(current->Lease.Id), "Water display credit could not cancel");
        return false;
    }
    const auto& input = metadata.Input;
    const auto transform = Reference ? DisplayTransform : input.Instance().ContainerToWorld;
    const auto makeView = [&](const ReadLease& read)
    {
        return GDX12FluidDisplay::View{read.View.Resource, SRVs.at(read.Lease.View.LocalSlot)->GPUHandle};
    };
    GDX12FluidDisplay display;
    display.Current = makeView(*current);
    display.Settings = input.Settings();
    display.Transform = transform;
    display.Count = Slot->Buffers.Count;
    display.HistoryValid = History && HistoryConsecutive;
    if (display.HistoryValid)
    {
        display.Previous = makeView(History->Read);
        display.PreviousTransform = History->Transform;
    }
    else ClearHistory();

    Frames[*frame].emplace(FrameUse{{std::move(*current), transform}, std::move(History),
        {std::move(*next), transform}, {}});
    History.reset();
    AwaitingTail = *frame;
    Selected = std::move(display);
    metadata.Displayed = true;
    Counters.SnapshotAgeSeconds = std::chrono::duration<double>(HostClock::now() - metadata.Captured).count();
    Counters.DisplaySimulationLag = (std::max)(0.0, Time->GetProgress().CompletedTime - metadata.Time);
    Counters.OverAge = MaxAgeMs > 0 && Counters.SnapshotAgeSeconds * 1000 > MaxAgeMs;
    if (!Time->GetProgress().FirstMissingInput)
        Counters.Status = Counters.OverAge ? "over-age" : "displaying";
    return true;
}

void CrossAdapterWater::Tick(const GDX12FluidInstance* instance, const GDX12FluidControls& controls,
    RenderMGPURuntime* runtime, GDX12DeviceResources& primary, GDX12Device* secondary, bool surfaceEligible)
{
    auto& s = *_state;
    Require(!s.AwaitingTail, "Previous water frame did not receive its tail receipt");
    s.Selected.reset();
    s.DemandExact = false;
    s.Counters.Requested = controls.MultiGPU;
    s.Counters.OverAge = false;
    s.Counters.SnapshotAgeSeconds = -1;
    s.Counters.DisplaySimulationLag = 0;
    if (s.Bank)
    {
        Require(s.Runtime == runtime && s.Primary == &primary && s.Secondary == secondary,
            "Water bank cannot move to another runtime or adapter in place");
        s.Collect();
    }

    const auto owner = instance ? instance->Id : 0;
    if (owner != s.Owner || (instance &&
        (instance->Component.GetResetRevision() != s.ResetRevision ||
         instance->Component.GetParticles() != s.Seed ||
         instance->Component.GetSettings().MGPUReference != s.Reference)))
        s.BeginEpoch(instance, controls, runtime, primary, secondary);
    s.Counters.ResetRequired = owner && (controls.MultiGPU != s.RequestedRoute ||
        (controls.MultiGPU && (!s.SecondaryOwns || controls.MultiGPUExact != s.RequestedExact)));
    if (!s.SecondaryOwns) return;

    s.Input.emplace(*instance, controls.Quality);
    s.DisplayTransform = instance->ContainerToWorld;
    const auto& settings = s.Input->Settings();
    const bool visible = controls.Enabled && settings.RenderEnabled && surfaceEligible;
    s.PublishCurrent |= visible && !s.Visible;
    s.Visible = visible;
    s.Enabled = controls.Enabled && !instance->Paused && (settings.SimulationEnabled || s.Visible);
    s.MaxAgeMs = controls.MultiGPUMaxAgeMs;
    if (controls.Enabled)
        s.Time->Append(*s.Input, s.Input->Elapsed(), s.Input->LogicalTick());
    const bool gap = s.Time->GetProgress().FirstMissingInput != 0;
    s.Counters.ResetRequired |= gap;
    s.Counters.Status = gap ? "input-history-full" : "awaiting-snapshot";
    if (!s.Configure())
    {
        if (s.SecondaryOwns && !gap) s.Counters.Status = "retiring-epoch";
        return;
    }
    if (s.Prepared && (!s.Visible || gap) && s.Prepared->Publishing) s.CancelSolve();
    s.Attempt = controls.MultiGPUUpdateHz == 0 || s.LastProducer == HostClock::time_point{} ||
        std::chrono::duration<double>(HostClock::now() - s.LastProducer).count() >= 1.0 / controls.MultiGPUUpdateHz;
    s.AdvanceOutputs();
    if (s.Exact && s.Visible && !gap)
    {
        s.DemandExact = s.Enabled && (s.Prepared.has_value() ||
            (s.Attempt && (s.PublishCurrent || s.Head.SeedPending || s.Time->Prepare().Size() ||
                (s.SolverDone.IsValid() && s.Time->GetProgress().AcceptedDebt > 0))));
        if (s.Prepared && !s.Prepared->Exact && s.Enabled) s.SubmitSolve(false);
    }
    else s.SubmitSolve(false);
    s.AdvanceOutputs();
}

void CrossAdapterWater::SelectFrame(FrameSubmissionId frame)
{
    auto& s = *_state;
    if (!s.SecondaryOwns || !s.LayoutReady || !s.Visible || s.DemandExact || s.AwaitingTail) return;
    const auto snapshot = s.Exact ? (s.History ? s.History->Read.Lease.View : Snapshot{}) :
        s.Bank->SelectLatest(frame).Current;
    if (!snapshot.IsValid()) return;
    auto& metadata = s.Local.at(snapshot.LocalSlot);
    Require(metadata && metadata->Version == snapshot.Version, "Selected water snapshot has no matching metadata");
    s.Select(snapshot.Publication, *metadata);
}

bool CrossAdapterWater::WantsExactBoundary() const
{
    return _state->DemandExact;
}

std::optional<CompletionPoint> CrossAdapterWater::SubmitExact(CompletionPoint prefix)
{
    auto& s = *_state;
    if (!s.DemandExact) return {};
    Require(prefix.IsValid() && prefix.Timeline() == s.Direct().GetTimelineIdentity() &&
        prefix.Value() == s.Direct().GetLastCompletionPoint().Value(), "Water Exact requires the issued primary prefix");
    auto receipt = s.SubmitSolve(true);
    s.DemandExact = false;
    if (!receipt)
    {
        ++s.Counters.ExactDeferrals;
        s.Counters.Status = "exact-deferred";
    }
    return receipt;
}

void CrossAdapterWater::RetireFrame(CompletionPoint tail)
{
    auto& s = *_state;
    if (!s.AwaitingTail)
    {
        s.HistoryConsecutive = false;
        return;
    }
    auto& frame = *s.Frames[*s.AwaitingTail];
    Require(tail.IsValid() && tail.Timeline() == s.Direct().GetTimelineIdentity() &&
        tail.Value() == s.Direct().GetLastCompletionPoint().Value(), "Water requires the issued primary frame tail");
    Require(s.Bank->BindReader(frame.Current.Read.Lease.Id, tail), "Water current display tail rejected");
    if (frame.Previous)
        Require(s.Bank->BindReader(frame.Previous->Read.Lease.Id, tail), "Water previous display tail rejected");
    s.History.emplace(std::move(frame.NextHistory));
    s.HistoryConsecutive = true;
    frame.Tail = tail;
    s.AwaitingTail.reset();
}

std::uint64_t CrossAdapterWater::RoutedOwner() const
{
    return _state->SecondaryOwns ? _state->Owner : 0;
}

const std::optional<GDX12FluidDisplay>& CrossAdapterWater::Display() const
{
    return _state->Selected;
}

CrossAdapterWater::Diagnostics CrossAdapterWater::GetDiagnostics() const
{
    const auto& s = *_state;
    auto result = s.Counters;
    result.SecondaryOwns = s.SecondaryOwns;
    result.Exact = s.Exact;
    if (s.Time)
    {
        const auto time = s.Time->GetProgress();
        result.Desired = time.DesiredTime;
        result.Issued = time.IssuedTime;
        result.Completed = time.CompletedTime;
        result.AcceptedDebt = time.AcceptedDebt;
        result.InFlight = time.IssuedTime - time.CompletedTime;
        result.Unrepresented = time.UnrepresentedElapsed;
        result.FirstMissingInput = time.FirstMissingInput;
    }
    return result;
}

#include "Rendering/Grass/CrossAdapterGrass.h"

#include "App.Base/Rendering/RenderMGPURuntime.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12DeviceResources.h"
#include "Engine.RendererDX12/GDX12SnapshotTransport.h"
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include <algorithm>
#include <deque>
#include <limits>
#include <stdexcept>

namespace
{
    using namespace GDX12Submission;
    using GDX12Snapshot::PhysicalView;
    using GDX12Snapshot::Result;
    using GDX12Snapshot::Ticket;
    using GDX12Snapshot::Transport;
    using Graph = GDX12RenderGraph;
    using Producer = GDX12GrassFieldProducer;
    using Recorder = std::function<void(GDX12CommandList&, const Graph::BoundResources&)>;

    void Require(bool result, const char* message)
    {
        if (!result) throw std::logic_error(message);
    }

    class Batch final : public GDX12RenderPass
    {
    public:
        Batch(const char* name, Placement placement, std::vector<Graph::Access> accesses, Recorder recorder)
            : _name(name), _placement(placement), _accesses(std::move(accesses)), _recorder(std::move(recorder)) {}
        bool SupportsGraphQueue(QueueKind queue) const override { return queue == _placement.Queue; }
        void RecordRenderGraph(GDX12RenderGraphBuilder& graph) override
        {
            graph.SetName(_name);
            graph.SetPlacement(_placement);
            graph.DeclareCompleteAccesses();
            graph.HasSideEffects();
            for (const auto& access : _accesses)
            {
                if (access.Version.IsValid()) graph.UseVersion(access.Id, access.Usage, access.Version, access.State);
                else graph.Use(access.Id, access.Usage, access.State);
            }
        }
        void RecordPlacedCommands(GDX12CommandList* list, const Graph::BoundResources& views) override
        { _recorder(*list, views); }
    private:
        const char* _name;
        Placement _placement;
        std::vector<Graph::Access> _accesses;
        Recorder _recorder;
    };

    struct Plan
    {
        std::vector<std::unique_ptr<Batch>> Passes;
        Graph Work;
        Graph::ExecutionBindings Bindings;

        Graph::Resource View(const char* name, const PhysicalView& view, bool external = false, bool shared = false)
        {
            const auto id = external ? Work.Import(name) : Work.Create(name);
            Work.RegisterResource(id, view.Identity, shared);
            Bindings.Views.push_back({id, view.Identity, view.Resource});
            return id;
        }
        Graph::Pass Add(const char* name, Placement lane, std::vector<Graph::Access> accesses, Recorder recorder)
        {
            Passes.push_back(std::make_unique<Batch>(name, lane, std::move(accesses), std::move(recorder)));
            return Work.AddPass(*Passes.back());
        }
        CompletionPoint Point(const Graph::ExecutionReceipt& receipt, Graph::Pass pass) const
        {
            const auto& batches = Work.GetCompiledPlan().Batches;
            const auto it = std::find_if(batches.begin(), batches.end(), [&](const auto& batch)
            { return batch.Passes.front() == pass; });
            Require(it != batches.end(), "Grass placed batch has no receipt");
            return receipt.Batches.at(static_cast<size_t>(it->Id));
        }
    };

    Graph::Pass Export(Plan& plan, Placement lane, Transport& bank, Ticket ticket, TransferVersion version,
        Graph::Resource source, Graph::Resource shared)
    {
        return plan.Add("Grass export", lane,
            {{source, Graph::Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE, version},
             {shared, Graph::Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST, version}},
            [&bank, ticket](GDX12CommandList& list, const Graph::BoundResources&)
            { bank.RecordExportCopies(ticket, *list.GetCommandList().Get()); });
    }

    Graph::Pass Import(Plan& plan, Placement lane, Transport& bank, Ticket ticket, TransferVersion version,
        Graph::Resource shared, Graph::Resource local)
    {
        return plan.Add("Grass import", lane,
            {{shared, Graph::Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE, version},
             {local, Graph::Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST, version}},
            [&bank, ticket](GDX12CommandList& list, const Graph::BoundResources&)
            { bank.RecordImportCopies(ticket, *list.GetCommandList().Get()); });
    }
}

struct CrossAdapterGrass::Impl
{
    struct Publication
    {
        TransferVersion Version;
        GDX12Snapshot::Reservation Credit;
        GDX12GrassLatestSelection::Stamp Stamp;
        PhysicalView Source;
        std::optional<CompletionPoint> ExportInput;
        CompletionPoint Exported;
        std::optional<GDX12Snapshot::ImportPlan> ImportPlan;
        CompletionPoint Ready;
        bool ExactPin = false;
    };
    struct Display
    {
        std::shared_ptr<const GDX12GrassFieldSnapshot> Snapshot;
        LocalView View;
        Ticket TicketId;
        uint64_t Pin = 0;
        GDX12GrassLatestSelection::Stamp Stamp;
        CompletionPoint Ready;
    };
    struct PendingDisplay
    {
        Display Selected;
        GDX12GrassLatestSelection Cursor;
    };

    RenderMGPURuntime& Runtime;
    GDX12DeviceResources& Primary;
    GDX12Device& Secondary;
    const Placement DirectA, CopyA, ComputeB, CopyB;
    const TransferId SeedStream, FieldStream;
    Epoch ActiveEpoch = Epoch::Invalid;
    D3D12_RESOURCE_DESC Desc = {};
    bool SeedConfigured = false, FieldConfigured = false;
    std::shared_ptr<Transport> SeedBank, FieldBank;
    std::shared_ptr<GDX12DescriptorHeap> SecondaryHeap;
    std::shared_ptr<Producer> Solver, ReturningSolver;
    std::array<CompletionPoint, GDX12Snapshot::SlotCount> SolverBusy;
    std::array<PhysicalView, GDX12Snapshot::SlotCount> Sources;
    std::array<Ticket, GDX12Snapshot::SlotCount> SourceUsers, LocalUsers;
    std::deque<Publication> Publications;
    Display Shown;
    std::shared_ptr<const GDX12GrassRenderer::LocalCheckpoint> Anchor;
    PhysicalView AnchorView;
    std::optional<GDX12GrassLatestSelection> Latest;
    std::optional<PendingDisplay> Pending;
    GDX12GrassFieldInputQueue Inputs;
    Tick Authority;
    CompletionPoint ReturnReady;
    std::vector<uint64_t> FrameReaders;
    std::vector<std::shared_ptr<GDX12Resource>> FrameResources;
    FrameId SelectedFrame = FrameId::Invalid;
    uint64_t Invocation = 0;
    GDX12GrassMGPUControls Controls;
    Clock::time_point LastSolve = {};
    bool IssuedSolve = false;
    State Route = State::Primary;

    Impl(RenderMGPURuntime& runtime, GDX12DeviceResources& primary, GDX12Device& secondary)
        : Runtime(runtime), Primary(primary), Secondary(secondary),
          DirectA{primary.Device->GetSubmissionAdapterId(), QueueKind::Direct},
          CopyA{DirectA.Adapter, QueueKind::Copy},
          ComputeB{secondary.GetSubmissionAdapterId(), QueueKind::Compute},
          CopyB{ComputeB.Adapter, QueueKind::Copy},
          SeedStream(runtime.NewTransferId()), FieldStream(runtime.NewTransferId())
    {
        FrameReaders.reserve(2);
        FrameResources.reserve(2);
    }

    GDX12CommandQueue& Queue(Placement lane) const
    {
        auto* queue = Runtime.GetQueue(lane);
        Require(queue != nullptr, "Required grass lane is unavailable");
        return *queue;
    }

    TransferVersion Version(TransferId stream)
    {
        Require(Invocation != (std::numeric_limits<uint64_t>::max)(), "Grass invocation id exhausted");
        return {stream, ActiveEpoch, static_cast<ProducerInvocationId>(++Invocation)};
    }

    PhysicalView View(std::shared_ptr<GDX12Resource> resource, Placement lane, TransferId stream = TransferId::Invalid)
    {
        return {std::move(resource), {Runtime.NewResourceId(), lane.Adapter, ActiveEpoch, stream, Runtime.NewAllocationId()}};
    }

    PhysicalView Source(Placement lane, TransferId stream)
    {
        auto& device = lane.Adapter == DirectA.Adapter ? *Primary.Device : Secondary;
        auto resource = std::make_shared<GDX12Resource>();
        resource->D3DResource = device.CreateResource(D3D12_HEAP_TYPE_DEFAULT, Desc, D3D12_RESOURCE_STATE_COMMON);
        return View(std::move(resource), lane, stream);
    }

    bool Configure(const Producer::SnapshotLayout& layout)
    {
        const auto size = UINT64(layout.ElementCount) * sizeof(XMFLOAT4);
        if (ActiveEpoch == Epoch::Invalid) ActiveEpoch = Runtime.NewEpoch();
        else if (Desc.Width != size && (SeedConfigured || FieldConfigured))
        {
            ActiveEpoch = Runtime.NewEpoch();
            if (SeedBank) Require(SeedBank->Reset(ActiveEpoch), "Grass seed layout reset failed");
            if (FieldBank) Require(FieldBank->Reset(ActiveEpoch), "Grass field layout reset failed");
            SeedConfigured = FieldConfigured = false;
        }
        Desc = CD3DX12_RESOURCE_DESC::Buffer(size);
        if (!SeedConfigured)
        {
            if (!SeedBank) SeedBank = Runtime.CreateBank(SeedStream, ActiveEpoch, DirectA.Adapter, ComputeB.Adapter, Desc);
            else if (!Runtime.TryReconfigureBank(*SeedBank, Desc)) return false;
            SeedConfigured = true;
        }
        if (!FieldConfigured)
        {
            if (!FieldBank) FieldBank = Runtime.CreateBank(FieldStream, ActiveEpoch, ComputeB.Adapter, DirectA.Adapter, Desc);
            else if (!Runtime.TryReconfigureBank(*FieldBank, Desc)) return false;
            FieldConfigured = true;
        }
        return true;
    }

    bool Start(std::shared_ptr<const GDX12GrassRenderer::LocalCheckpoint> checkpoint,
        const Tick& input, CompletionPoint ready);
    bool Solve(Clock::time_point now);
    void PumpCopies();
    void StartReturn();
    std::optional<Frame> Choose(FrameId frame, uint64_t demanded, Clock::time_point now, bool allowPendingExact);
    void FinishFrame(std::optional<CompletionPoint> tail);
    void Clear();
    void ReleaseSupersededExact();
    void PruneRecycledMetadata();
};

bool CrossAdapterGrass::Impl::Start(std::shared_ptr<const GDX12GrassRenderer::LocalCheckpoint> checkpoint,
    const Tick& input, CompletionPoint ready)
{
    if (Route != State::Primary || !checkpoint || !checkpoint->Packed || !input.Sequence || Runtime.IsStopped() ||
        !Runtime.GetQueue(ComputeB) || !Runtime.GetQueue(CopyA) || !Runtime.GetQueue(CopyB)) return false;
    auto& direct = Queue(DirectA);
    if (!ready.IsValid() || !(ready.Timeline() == direct.GetTimelineIdentity()) ||
        ready.Value() > direct.GetLastCompletionPoint().Value() ||
        checkpoint->Packed->GetCurrentState() != D3D12_RESOURCE_STATE_COMMON) return false;
    if (!checkpoint->Layout.Matches(input.Inputs()) || !Configure(checkpoint->Layout)) return false;

    if (!SecondaryHeap)
        SecondaryHeap = std::make_shared<GDX12DescriptorHeap>(&Secondary,
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2048, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
    auto solver = std::make_shared<Producer>(Secondary, *SecondaryHeap, SolverBusy.size());
    solver->PrepareSeed(0, input.Inputs(), checkpoint->Layout);
    auto staged = Source(DirectA, SeedStream);
    const auto version = Version(SeedStream);
    auto& bank = *SeedBank;
    const auto credit = bank.Reserve(version, SnapshotReadMode::ExactVersion, direct,
        staged.Resource->D3DResource, staged.Identity.Allocation);
    if (credit.Status == Result::Deferred) bank.CancelDeferredExact(version);
    if (credit.Status != Result::Accepted) return false;
    const auto ticket = credit.Publication;
    if (!bank.PrepareExact(ticket))
    {
        Require(bank.Cancel(ticket), "Grass seed reservation could not be cancelled");
        return false;
    }
    auto reader = bank.PrepareExactReader(ticket, Queue(ComputeB));
    if (!reader)
    {
        Require(bank.Cancel(ticket), "Grass seed reader reservation could not be cancelled");
        return false;
    }

    bool issued = false;
    try
    {
        auto localSnapshot = std::make_shared<GDX12GrassFieldSnapshot>(*Primary.SRV_UAV_Heap,
            *checkpoint->Packed, checkpoint->Layout, checkpoint->Packed);
        const auto localCheckpoint = View(checkpoint->Packed, DirectA);
        const auto& shared = bank.GetSlots().at(credit.SharedSlot);
        Plan plan;
        plan.Bindings.KeepAlive = {checkpoint, solver, SecondaryHeap, SeedBank};
        const auto checkpointView = plan.View("Grass authoritative primary checkpoint", localCheckpoint, true);
        const auto source = plan.View("Grass seed source", staged);
        const auto remoteExport = plan.View("Grass seed shared export", shared.Export, false, true);
        const auto remoteImport = plan.View("Grass seed shared import", shared.Import, false, true);
        const auto local = plan.View("Grass secondary seed", reader->View);
        const auto stage = plan.Add("Stage grass seed", DirectA,
            {{checkpointView, Graph::Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE},
             {source, Graph::Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST, version}},
            [=](GDX12CommandList& commands, const Graph::BoundResources& views)
            { commands.GetCommandList()->CopyBufferRegion(views.Get(source), 0, views.Get(checkpointView), 0, Desc.Width); });
        const auto exported = Export(plan, CopyA, bank, ticket, version, source, remoteExport);
        const auto imported = Import(plan, CopyB, bank, ticket, version, remoteImport, local);
        const auto seeded = plan.Add("Seed grass secondary solver", ComputeB,
            {{local, Graph::Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE, version}},
            [=](GDX12CommandList& commands, const Graph::BoundResources& views)
            { solver->RecordSeed(commands, 0, *views.Get(local)); });
        plan.Work.DependsOn(exported, stage);
        plan.Work.DependsOn(imported, exported);
        plan.Work.DependsOn(seeded, imported);
        plan.Work.CompilePlan();
        const auto receipt = Runtime.TrySubmit(plan.Work, std::move(plan.Bindings), [&](const auto& submitted)
        {
            const auto ep = plan.Point(submitted, exported), ip = plan.Point(submitted, imported);
            bank.CommitExact(ticket, plan.Point(submitted, stage), ep, ep, ip, ip);
            SolverBusy[0] = plan.Point(submitted, seeded);
            Require(bank.BindReader(reader->Id, SolverBusy[0]) && bank.ReleaseExact(ticket), "Grass seed lease binding failed");
            solver->Commit(0);
        });
        issued = receipt.has_value();
        if (!receipt)
        {
            Require(bank.Cancel(ticket), "Grass seed capacity retry could not cancel");
            return false;
        }
        Solver = std::move(solver);
        Authority = input;
        Anchor = std::move(checkpoint);
        AnchorView = localCheckpoint;
        Shown = {std::move(localSnapshot), {AnchorView.Identity, AnchorView.Resource, {}}, {}, 0,
            {input.Sequence, Anchor->Layout, input.CapturedAt}, {}};
        Latest.emplace(FieldStream, ActiveEpoch, Shown.Stamp);
        Inputs.StartAfter(input.Sequence);
        Route = State::Secondary;
        return true;
    }
    catch (...)
    {
        if (!issued && !Runtime.IsStopped()) bank.Cancel(ticket);
        throw;
    }
}

bool CrossAdapterGrass::Impl::Solve(Clock::time_point now)
{
    if (Route != State::Secondary || !Inputs.Size()) return false;
    if (Controls.UpdateHz > 0 && IssuedSolve &&
        std::chrono::duration<double>(now - LastSolve).count() < 1.0 / Controls.UpdateHz) return false;
    auto& compute = Queue(ComputeB);
    const auto progress = compute.GetCompletionProgress();
    size_t slot = 0;
    for (; slot < Sources.size(); ++slot)
        if ((!SolverBusy[slot].IsValid() || SolverBusy[slot].IsReachedBy(progress)) &&
            (!SourceUsers[slot].Id() || FieldBank->SourceReusable(SourceUsers[slot]))) break;
    if (slot == Sources.size()) return false;

    const size_t count = (std::min)(Inputs.Size(), Producer::MaxStepsPerInvocation);
    std::vector<Producer::Inputs> steps;
    for (size_t i = 0; i < count; ++i) steps.push_back(Inputs.At(i).Inputs());
    const auto last = Inputs.At(count - 1);
    const auto version = Version(FieldStream);
    if (!Sources[slot].Resource) Sources[slot] = Source(ComputeB, FieldStream);
    const auto& source = Sources[slot];
    const bool exact = Controls.Exact && count == Inputs.Size();
    auto& bank = *FieldBank;
    const auto credit = bank.Reserve(version, exact ? SnapshotReadMode::ExactVersion : SnapshotReadMode::LatestCompleted,
        compute, source.Resource->D3DResource, source.Identity.Allocation);
    if (credit.Status == Result::Deferred && exact) bank.CancelDeferredExact(version);
    if (credit.Status != Result::Accepted) return false;
    const auto ticket = credit.Publication;
    if (exact) LocalUsers.at(credit.LocalSlot) = ticket;
    if (exact && !bank.PrepareExact(ticket))
    {
        Require(bank.Cancel(ticket), "Grass exact preparation rollback failed");
        return false;
    }
    bool staged = false;
    bool issued = false;
    try
    {
        Solver->PrepareBatch(static_cast<UINT>(slot), steps.data(), steps.size());
        staged = true;
        const auto& layout = Solver->Snapshot(static_cast<UINT>(slot));
        Publication publication{version, credit, {last.Sequence, layout, last.CapturedAt}, source};
        Plan plan;
        plan.Bindings.KeepAlive = {Solver, SecondaryHeap, FieldBank};
        const auto sourceView = plan.View("Grass secondary packed output", source);
        const auto solve = plan.Add("Solve and pack grass fields", ComputeB,
            {{sourceView, Graph::Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST, version}},
            [&](GDX12CommandList& commands, const Graph::BoundResources& views)
            { Solver->Record(commands, static_cast<UINT>(slot), views.Get(sourceView)); });
        Graph::Pass exported = 0, imported = 0;
        if (exact)
        {
            const auto& shared = bank.GetSlots().at(credit.SharedSlot);
            const auto ex = plan.View("Grass exact shared export", shared.Export, false, true);
            const auto im = plan.View("Grass exact shared import", shared.Import, false, true);
            const auto local = plan.View("Grass exact local field", bank.GetSlots().at(credit.LocalSlot).Local);
            exported = Export(plan, CopyB, bank, ticket, version, sourceView, ex);
            imported = Import(plan, CopyA, bank, ticket, version, im, local);
            plan.Work.DependsOn(exported, solve);
            plan.Work.DependsOn(imported, exported);
        }
        plan.Work.CompilePlan();
        const auto receipt = Runtime.TrySubmit(plan.Work, std::move(plan.Bindings), [&](const auto& submitted)
        {
            SolverBusy[slot] = plan.Point(submitted, solve);
            if (exact)
            {
                publication.Exported = plan.Point(submitted, exported);
                publication.Ready = plan.Point(submitted, imported);
                bank.CommitExact(ticket, SolverBusy[slot], publication.Exported, publication.Exported,
                    publication.Ready, publication.Ready);
                publication.ExactPin = true;
            }
            else Require(bank.BindProducer(ticket, SolverBusy[slot]), "Grass Latest producer receipt binding failed");
            SourceUsers[slot] = ticket;
            Solver->Commit(static_cast<UINT>(slot));
            for (size_t i = 0; i < count; ++i) Inputs.Commit(Inputs.Front()->Sequence);
            Authority = last;
        });
        issued = receipt.has_value();
        if (!receipt)
        {
            Require(bank.Cancel(ticket), "Grass solver capacity retry could not cancel");
            Solver->Cancel(static_cast<UINT>(slot));
            return false;
        }
        Publications.push_back(std::move(publication));
        LastSolve = now;
        IssuedSolve = true;
        return true;
    }
    catch (...)
    {
        if (!issued && !Runtime.IsStopped())
        {
            bank.Cancel(ticket);
            if (staged) Solver->Cancel(static_cast<UINT>(slot));
        }
        throw;
    }
}

void CrossAdapterGrass::Impl::PumpCopies()
{
    auto& bank = *FieldBank;
    for (auto& pending : Publications)
    {
        if (pending.Ready.IsValid()) continue;
        const auto ticket = pending.Credit.Publication;
        const auto version = pending.Version;
        const auto& shared = bank.GetSlots().at(pending.Credit.SharedSlot);
        if (!pending.Exported.IsValid())
        {
            if (!pending.ExportInput) pending.ExportInput = bank.PrepareExport(ticket);
            if (!pending.ExportInput) continue;
            Plan plan;
            plan.Bindings.KeepAlive = {FieldBank};
            const auto source = plan.View("Grass Latest packed source", pending.Source, true);
            plan.Work.ImportVersion(source, version);
            plan.Bindings.ExactInputs.push_back({source, version, *pending.ExportInput, FieldBank});
            const auto exportView = plan.View("Grass Latest shared export", shared.Export, false, true);
            const auto pass = Export(plan, CopyB, bank, ticket, version, source, exportView);
            plan.Work.CompilePlan();
            const auto submitted = Runtime.TrySubmit(plan.Work, std::move(plan.Bindings), [&](const auto& receipt)
            {
                pending.Exported = plan.Point(receipt, pass);
                bank.CommitExport(ticket, pending.Exported, pending.Exported);
            });
            if (!submitted) break;
        }
        if (!pending.ImportPlan)
        {
            const auto import = bank.PrepareImport(ticket);
            if (import.Status != Result::Accepted) continue;
            LocalUsers.at(import.LocalSlot) = ticket;
            pending.ImportPlan = import;
        }
        // PrepareImport admits Latest only after Poll observed completion of the secondary export.
        const auto& import = *pending.ImportPlan;
        Plan plan;
        plan.Bindings.KeepAlive = {FieldBank};
        const auto importView = plan.View("Grass Latest shared import", shared.Import, true, true);
        plan.Work.ImportVersion(importView, version);
        plan.Bindings.ExactInputs.push_back({importView, version, import.ExportReady, FieldBank,
            &Queue(CopyB), shared.Export.Identity});
        const auto local = plan.View("Grass Latest local field", bank.GetSlots().at(import.LocalSlot).Local);
        const auto pass = Import(plan, CopyA, bank, ticket, version, importView, local);
        plan.Work.CompilePlan();
        const auto submitted = Runtime.TrySubmit(plan.Work, std::move(plan.Bindings), [&](const auto& receipt)
        {
            pending.Ready = plan.Point(receipt, pass);
            bank.CommitImport(ticket, pending.Ready, pending.Ready);
        });
        if (!submitted) break;
    }
}

std::optional<CrossAdapterGrass::Frame> CrossAdapterGrass::Impl::Choose(FrameId frame, uint64_t demanded,
    Clock::time_point now, bool allowPendingExact)
{
    if (!Shown.Snapshot) return {};
    Require(frame != FrameId::Invalid && SelectedFrame == FrameId::Invalid, "Grass frame already selected");
    auto& bank = *FieldBank;
    const auto progress = Queue(CopyA).GetCompletionProgress();
    if (!allowPendingExact && Shown.Ready.IsValid() && !Shown.Ready.IsReachedBy(progress)) return {};
    const auto completed = bank.SelectLatest(frame).Current;
    auto candidate = Publications.end();
    if (allowPendingExact && Controls.Exact && Route == State::Secondary)
        candidate = std::find_if(Publications.begin(), Publications.end(), [&](const auto& item)
        { return item.ExactPin && item.Ready.IsValid() && item.Stamp.Sequence == demanded; });
    else if (completed.IsValid())
    {
        candidate = std::find_if(Publications.begin(), Publications.end(), [&](const auto& item)
        { return item.Credit.Publication == completed.Publication; });
        Require(candidate != Publications.end(), "Completed grass import has no publication metadata");
    }

    auto selection = *Latest;
    if (candidate != Publications.end() && selection.TryAdopt(candidate->Version, candidate->Stamp))
    {
        const auto ticket = candidate->Credit.Publication;
        auto pin = bank.AcquireReader(ticket, Queue(DirectA));
        if (!pin) return {};
        try
        {
            Display next{std::make_shared<GDX12GrassFieldSnapshot>(*Primary.SRV_UAV_Heap, *pin->View.Resource,
                candidate->Stamp.Layout, pin->View.Resource), {pin->View.Identity, pin->View.Resource, candidate->Version},
                ticket, pin->Lease.Id, candidate->Stamp, candidate->Ready};
            Pending.emplace(PendingDisplay{std::move(next), std::move(selection)});
        }
        catch (...)
        {
            bank.CancelReader(pin->Lease.Id);
            throw;
        }
    }

    const auto& selected = Pending ? Pending->Selected : Shown;
    const auto acquire = [&](const Display& display)
    {
        if (!display.TicketId.Id()) return true;
        auto reader = bank.AcquireReader(display.TicketId, Queue(DirectA));
        if (!reader) return false;
        FrameReaders.push_back(reader->Lease.Id);
        return true;
    };
    if (!acquire(selected) || (Pending && !acquire(Shown)))
    {
        FinishFrame(std::nullopt);
        return {};
    }

    SelectedFrame = frame;
    FrameResources.push_back(selected.View.Resource);
    if (Shown.View.Resource != selected.View.Resource) FrameResources.push_back(Shown.View.Resource);
    const auto& cursor = Pending ? Pending->Cursor : *Latest;
    Frame result;
    result.Fields = {selected.Snapshot, Shown.Snapshot, Pending && !Shown.TicketId.Id()};
    result.CurrentView = selected.View;
    result.PreviousView = Shown.View;
    if (selected.Ready.IsValid() && !selected.Ready.IsReachedBy(progress)) result.Ready = selected.Ready;
    result.Sequence = selected.Stamp.Sequence;
    result.Exact = Controls.Exact && Route == State::Secondary && selected.TicketId.Id() && result.Sequence == demanded;
    result.AgeMs = cursor.AgeMs(now);
    result.OverAge = cursor.OverAge(now, Controls.MaxAgeMs);
    return result;
}

void CrossAdapterGrass::Impl::FinishFrame(std::optional<CompletionPoint> tail)
{
    if (tail)
    {
        if (SelectedFrame == FrameId::Invalid) return;
        Require(tail->IsValid() && tail->Timeline() == Queue(DirectA).GetTimelineIdentity(),
            "Grass frame retirement must be the joined primary DIRECT tail");
    }
    for (const auto reader : FrameReaders)
        Require(tail ? FieldBank->BindReader(reader, *tail) : FieldBank->CancelReader(reader),
            "Grass frame reader could not retire");

    const auto ticket = Pending ? Pending->Selected.TicketId : Shown.TicketId;
    if (tail && ticket.Id())
    {
        const auto publication = std::find_if(Publications.begin(), Publications.end(), [&](const auto& item)
        { return item.Credit.Publication == ticket; });
        if (publication != Publications.end() && publication->ExactPin)
        {
            Require(FieldBank->ReleaseExact(ticket), "Grass exact field did not bind its frame reader");
            publication->ExactPin = false;
        }
    }
    if (Pending)
    {
        if (tail)
        {
            // The previous field now has a real tail reader. Its display pin can be replaced.
            if (Shown.Pin) Require(FieldBank->CancelReader(Shown.Pin), "Previous grass display pin could not retire");
            Shown = std::move(Pending->Selected);
            *Latest = std::move(Pending->Cursor);
        }
        else
            Require(FieldBank->CancelReader(Pending->Selected.Pin), "Unsubmitted grass display pin could not cancel");
        Pending.reset();
    }
    FrameReaders.clear();
    FrameResources.clear();
    SelectedFrame = FrameId::Invalid;
}

void CrossAdapterGrass::Impl::ReleaseSupersededExact()
{
    auto& bank = *FieldBank;
    for (auto& publication : Publications)
    {
        if (!publication.ExactPin || publication.Stamp.Sequence >= Authority.Sequence ||
            publication.Credit.Publication == Shown.TicketId) continue;
        // A later accepted input supersedes this unused exact demand. Install and cancel a CPU
        // lease before dropping the demand pin. No native reader was issued for this lease.
        auto abandoned = bank.AcquireReader(publication.Credit.Publication, Queue(DirectA));
        if (!abandoned) continue;
        Require(bank.ReleaseExact(publication.Credit.Publication) && bank.CancelReader(abandoned->Lease.Id),
            "Unused grass exact demand could not retire");
        publication.ExactPin = false;
    }
}

void CrossAdapterGrass::Impl::PruneRecycledMetadata()
{
    // A new local credit proves the former import cannot be selected again. Pending
    // publications and completed imports still occupying a local slot retain their layouts.
    const auto end = std::remove_if(Publications.begin(), Publications.end(), [&](const auto& publication)
    {
        const auto slot = publication.ImportPlan ? publication.ImportPlan->LocalSlot : publication.Credit.LocalSlot;
        return slot != GDX12Snapshot::NoSlot && !(LocalUsers.at(slot) == publication.Credit.Publication);
    });
    Publications.erase(end, Publications.end());
}

void CrossAdapterGrass::Impl::StartReturn()
{
    if (ReturnReady.IsValid()) return;
    auto& bank = *FieldBank;
    std::optional<GDX12Snapshot::ReadLease> read;
    PhysicalView source;
    auto publication = Publications.end();
    if (Authority.Sequence == Shown.Stamp.Sequence && !Shown.TicketId.Id())
        source = AnchorView;
    else
    {
        publication = std::find_if(Publications.begin(), Publications.end(), [&](const auto& item)
        { return item.Stamp.Sequence == Authority.Sequence; });
        if (publication == Publications.end() || !publication->Ready.IsValid() ||
            !publication->Ready.IsReachedBy(Queue(CopyA).GetCompletionProgress())) return;
        read = bank.AcquireReader(publication->Credit.Publication, Queue(DirectA));
        if (!read) return;
        source = read->View;
    }
    if (source.Resource->GetCurrentState() != D3D12_RESOURCE_STATE_COMMON)
    {
        if (read) Require(bank.CancelReader(read->Lease.Id), "Grass return reader could not cancel");
        return;
    }
    bool issued = false;
    try
    {
        const auto& layout = publication == Publications.end() ? Anchor->Layout : publication->Stamp.Layout;
        auto local = std::make_shared<Producer>(*Primary.Device, *Primary.SRV_UAV_Heap, Primary.FrameConstants.size());
        local->PrepareSeed(0, Authority.Inputs(), layout);
        Plan plan;
        plan.Bindings.KeepAlive = {local, FieldBank};
        const auto sourceView = plan.View("Grass authoritative return checkpoint", source, true);
        const auto version = read ? read->Lease.View.Version : TransferVersion{};
        if (read)
        {
            plan.Work.ImportVersion(sourceView, version);
            plan.Bindings.ExactInputs.push_back({sourceView, version, read->Lease.View.ImportReady, FieldBank});
        }
        const auto seed = plan.Add("Seed grass primary solver", DirectA,
            {{sourceView, Graph::Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE, version}},
            [&](GDX12CommandList& list, const Graph::BoundResources& views)
            { local->RecordSeed(list, 0, *views.Get(sourceView)); });
        plan.Work.CompilePlan();
        const auto receipt = Runtime.TrySubmit(plan.Work, std::move(plan.Bindings), [&](const auto& submitted)
        {
            ReturnReady = plan.Point(submitted, seed);
            if (read)
            {
                Require(bank.BindReader(read->Lease.Id, ReturnReady), "Grass return reader could not bind");
                if (publication->ExactPin)
                {
                    Require(bank.ReleaseExact(publication->Credit.Publication), "Grass return exact pin could not release");
                    publication->ExactPin = false;
                }
            }
            local->Commit(0);
        });
        issued = receipt.has_value();
        if (!receipt)
        {
            if (read) Require(bank.CancelReader(read->Lease.Id), "Grass return capacity retry could not cancel");
            return;
        }
        ReturningSolver = std::move(local);
    }
    catch (...)
    {
        if (read && !issued && !Runtime.IsStopped()) bank.CancelReader(read->Lease.Id);
        throw;
    }
}

void CrossAdapterGrass::Impl::Clear()
{
    Require(SelectedFrame == FrameId::Invalid && FrameReaders.empty() && !Pending,
        "Retire grass readers before resetting the epoch");
    if (Shown.Pin) Require(FieldBank->CancelReader(Shown.Pin), "Grass display pin could not reset");
    for (const auto& publication : Publications)
        if ((publication.ExportInput && !publication.Exported.IsValid()) ||
            (publication.ImportPlan && !publication.Ready.IsValid()))
            Require(FieldBank->Cancel(publication.Credit.Publication), "Unsubmitted grass copy stage could not reset");
    if (ActiveEpoch != Epoch::Invalid)
    {
        ActiveEpoch = Runtime.NewEpoch();
        if (SeedBank) Require(SeedBank->Reset(ActiveEpoch), "Grass seed stream could not reset");
        if (FieldBank) Require(FieldBank->Reset(ActiveEpoch), "Grass display stream could not reset");
        SeedConfigured = FieldConfigured = false;
    }
    Publications.clear();
    Solver.reset();
    ReturningSolver.reset();
    SolverBusy = {};
    Sources = {};
    SourceUsers = {};
    LocalUsers = {};
    Shown = {};
    Anchor.reset();
    AnchorView = {};
    Latest.reset();
    Inputs.Reset();
    Authority = {};
    ReturnReady = {};
    LastSolve = {};
    IssuedSolve = false;
    Route = State::Primary;
}

CrossAdapterGrass::CrossAdapterGrass(RenderMGPURuntime& runtime, GDX12DeviceResources& primary, GDX12Device& secondary)
    : _impl(std::make_unique<Impl>(runtime, primary, secondary))
{}

CrossAdapterGrass::~CrossAdapterGrass() = default;

CrossAdapterGrass::State CrossAdapterGrass::GetState() const { return _impl->Route; }

bool CrossAdapterGrass::TryStart(std::shared_ptr<const GDX12GrassRenderer::LocalCheckpoint> checkpoint,
    const Tick& checkpointInput, CompletionPoint checkpointReady)
{
    return _impl->Start(std::move(checkpoint), checkpointInput, checkpointReady);
}

bool CrossAdapterGrass::Capture(Tick input)
{
    auto& state = *_impl;
    if ((state.Route != State::Secondary && state.Route != State::Returning) ||
        input.Sequence <= state.Authority.Sequence) return false;
    if (input.ResetWind || !state.Shown.Stamp.Layout.Matches(input.Inputs()))
    {
        state.Route = State::ResetRequired;
        return false;
    }
    return state.Inputs.Push(std::move(input));
}

void CrossAdapterGrass::Pump(const GDX12GrassMGPUControls& controls, Clock::time_point now)
{
    auto& state = *_impl;
    if (state.Route == State::Primary || state.Route == State::ResetRequired || state.Runtime.IsStopped()) return;
    Require(state.SelectedFrame == FrameId::Invalid, "Retire or cancel the grass selection before pumping another frame");
    if (!controls.Enabled) state.Route = State::Returning;
    const bool exactInFlight = state.Controls.Exact && state.Shown.Ready.IsValid() &&
        !state.Shown.Ready.IsReachedBy(state.Queue(state.CopyA).GetCompletionProgress());
    state.Controls = controls;
    if (exactInFlight) state.Controls.Exact = true;
    state.PumpCopies();
    state.ReleaseSupersededExact();
    if (state.Route == State::Returning)
    {
        if (state.Inputs.FirstMissing()) state.Route = State::ResetRequired;
        else state.StartReturn();
    }
    else
    {
        for (size_t attempt = 0; attempt < GDX12Snapshot::SlotCount && state.Solve(now); ++attempt)
            state.PumpCopies();
    }
    state.PruneRecycledMetadata();
}

std::optional<CrossAdapterGrass::Frame> CrossAdapterGrass::Select(FrameId frame, uint64_t demandedSequence,
    Clock::time_point now, bool allowPendingExact)
{
    if (_impl->Route == State::Primary || _impl->Route == State::ResetRequired) return {};
    return _impl->Choose(frame, demandedSequence, now, allowPendingExact);
}

void CrossAdapterGrass::RecordFrameRelease(GDX12CommandList& commands) const
{
    for (const auto& resource : _impl->FrameResources)
        commands.ResourceBarrier({resource->GetTransitionBarrier(D3D12_RESOURCE_STATE_COMMON)});
}

void CrossAdapterGrass::RetireFrame(CompletionPoint tail) { _impl->FinishFrame(tail); }
void CrossAdapterGrass::CancelFrame() { _impl->FinishFrame(std::nullopt); }

std::optional<CrossAdapterGrass::LocalContinuation> CrossAdapterGrass::TakeLocalContinuation()
{
    auto& state = *_impl;
    if (state.Runtime.IsStopped() || !state.ReturnReady.IsValid() || !state.ReturnReady.IsReachedBy(state.Queue(state.DirectA).GetCompletionProgress()))
        return {};
    LocalContinuation result{std::move(state.ReturningSolver), std::move(state.Inputs), state.Authority.Sequence,
        state.Authority.InteractionResetRevision};
    state.Inputs = {};
    state.Clear();
    return result;
}

void CrossAdapterGrass::Reset() { _impl->Clear(); }

CrossAdapterGrass::InputStatus CrossAdapterGrass::GetInputStatus() const
{
    const auto& state = *_impl;
    return {state.Authority.Sequence, state.Inputs.FirstMissing(), state.Inputs.PendingSeconds(),
        state.Inputs.MissingSeconds(), state.Inputs.SupersededSeconds()};
}

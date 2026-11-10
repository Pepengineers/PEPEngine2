#include "App.Base/Rendering/CrossAdapterEnvironment.h"
#include "App.Base/Rendering/RenderMGPURuntime.h"
#include "Common/ConsoleVariables.h"
#include "Engine.RendererDX12/GDX12SnapshotTransport.h"
#include <algorithm>
#include <functional>
#include <stdexcept>
#include <utility>

namespace
{
    using Graph = GDX12RenderGraph;
    using Input = GDX12CloudEnvironmentInputs;
    using namespace GDX12Submission;
    namespace Snapshot = GDX12Snapshot;

    AutoConsoleVariable<bool> BistroClouds(L"Render.Clouds.Bistro", false,
        L"Enable the procedural cloud source in Bistro.", EConsoleVariableFlags::Default);
    AutoConsoleVariable<bool> LocalCompute(L"Render.Clouds.AsyncCompute", false,
        L"Generate procedural cloud environment snapshots on primary COMPUTE.", EConsoleVariableFlags::Default);
    AutoConsoleVariable<bool> MultiGPU(L"Render.Clouds.MultiGPU", false,
        L"Generate procedural cloud environment snapshots on secondary COMPUTE when available.", EConsoleVariableFlags::Default);
    AutoConsoleVariable<bool> Latest(L"Render.Clouds.Latest", true,
        L"Show the latest completed cloud snapshot. False demands an exact snapshot.", EConsoleVariableFlags::Default);

    constexpr std::array<UINT, 3> CubeOutputs{ 0, 1, 3 };

    using View = Snapshot::PhysicalView;

    class Batch final : public GDX12RenderPass
    {
    public:
        using Recorder = std::function<void(GDX12CommandList*, const Graph::BoundResources&)>;
        Batch(const char* name, Placement placement, std::vector<Graph::Access> accesses, Recorder recorder)
            : _name(name), _placement(placement), _accesses(std::move(accesses)), _recorder(std::move(recorder)) {}

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

        bool SupportsGraphQueue(QueueKind queue) const override { return queue == _placement.Queue; }
        void RecordPlacedCommands(GDX12CommandList* list, const Graph::BoundResources& resources) override
        {
            _recorder(list, resources);
        }

    private:
        const char* _name;
        Placement _placement;
        std::vector<Graph::Access> _accesses;
        Recorder _recorder;
    };

    Graph::Resource Bind(Graph& graph, Graph::ExecutionBindings& bindings, const char* name,
        const View& view, bool imported, bool shared = false)
    {
        const auto id = imported ? graph.Import(name) : graph.Create(name);
        graph.RegisterResource(id, view.Identity, shared);
        bindings.Views.push_back({ id, view.Identity, view.Resource });
        return id;
    }

    bool Reached(const CompletionPoint point, GDX12CommandQueue& queue)
    {
        if (!point.IsValid()) return true;
        const auto progress = queue.GetCompletionProgress(point.Timeline());
        if (progress.CompletedValue == UINT64_MAX) ThrowIfFailed(DXGI_ERROR_DEVICE_REMOVED);
        return point.IsReachedBy(progress);
    }

    void Require(bool condition, const char* message)
    {
        if (!condition) throw std::logic_error(message);
    }
}

struct CrossAdapterEnvironment::State
{
    struct Sample
    {
        Input Inputs;
        uint64_t Invocation;
    };

    struct Slot
    {
        View Payload;
        UINT CubeSize = 0;
        std::optional<Sample> Result;
        CompletionPoint Produced;
        CompletionPoint Read;
    };

    struct RemoteSource
    {
        View Payload;
        Snapshot::Ticket Ticket;
        UINT CubeSize = 0;
    };

    struct Publication
    {
        Sample Data;
        Snapshot::Reservation Credit;
        TransferVersion Version;
        View Source;
        CompletionPoint Produced;
        CompletionPoint Exported;
    };

    struct RemoteRecorder
    {
        GDX12DeviceResources Resources;
        std::unique_ptr<GDX12EnvironmentIBLPass::CloudProducer> Recorder;

        RemoteRecorder(GDX12Device& device, const GDX12CloudEnvironmentLayout& layout)
        {
            Resources.Device = &device;
            Resources.SRV_UAV_Heap = std::make_unique<GDX12DescriptorHeap>(&device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                TextureCube_StartIndex + TextureCube_RangeLength, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
            Recorder = std::make_unique<GDX12EnvironmentIBLPass::CloudProducer>(Resources, layout);
        }
    };

    explicit State(GDX12DeviceResources& primary, GDX12Device* secondary, RenderMGPURuntime* runtime)
        : Primary(primary), Secondary(secondary), Runtime(runtime) {}

    GDX12DeviceResources& Primary;
    GDX12Device* Secondary;
    RenderMGPURuntime* Runtime;
    GDX12CloudComponent Cloud;
    bool Attached = false;
    bool UseLatest = true;
    Route Active = Route::Direct;
    bool RequestExact = false;
    GDX12CommandQueue* Compute = nullptr;
    std::optional<Input> Current, Admitted;
    std::optional<Sample> Installed;
    Slot* Selected = nullptr;
    std::array<Slot, 3> Slots;
    GDX12CloudCadence Cadence;
    GDX12CloudPublicationSettings Settings;
    Input::Clock::time_point Origin = Input::Clock::now();
    Diagnostics Stats;
    uint64_t IdentitySequence = 0;
    uint64_t Invocation = 0;
    uint64_t FrameSequence = 0;
    std::shared_ptr<const GDX12CloudEnvironmentLayout> Layout;
    std::shared_ptr<GDX12EnvironmentIBLPass::CloudProducer> Recorder;
    UINT RecorderSize = 0;
    CompletionPoint RecorderReady;
    std::array<std::weak_ptr<GDX12Resource>, 3> OutputOwners;
    std::array<ResourceIdentity, 3> OutputIds;
    std::shared_ptr<Snapshot::Transport> Bank;
    TransferId Stream = TransferId::Invalid;
    Epoch RemoteEpoch = Epoch::Invalid;
    bool BankNeedsLayout = false;
    std::array<RemoteSource, Snapshot::SlotCount> RemoteSources;
    std::array<std::optional<Publication>, Snapshot::SlotCount> Pending;
    std::array<std::optional<Sample>, Snapshot::SlotCount> Imported;
    Snapshot::Snapshot SelectedRemote;
    std::shared_ptr<RemoteRecorder> SecondaryRecorder;
    UINT SecondaryRecorderSize = 0;
    CompletionPoint SecondaryReady;
    // Destroy the executor first so its resources can retire while their heaps still exist.
    Graph::PlanExecutor Executor{ 12 };

    ResourceIdentity NewIdentity()
    {
        if (Runtime)
            return { Runtime->NewResourceId(), Primary.Device->GetSubmissionAdapterId(), Runtime->NewEpoch(),
                TransferId::Invalid, Runtime->NewAllocationId() };
        if (IdentitySequence == UINT64_MAX) throw std::overflow_error("Cloud resource identities exhausted");
        const auto id = ++IdentitySequence;
        return { ResourceId{id}, Primary.Device->GetSubmissionAdapterId(), Epoch{id},
            TransferId::Invalid, AllocationId{id} };
    }

    uint64_t NextInvocation() const
    {
        if (Invocation == UINT64_MAX) throw std::overflow_error("Cloud producer invocation exhausted");
        return Invocation + 1;
    }

    void Reset()
    {
        ++Stats.EpochResets;
        Installed.reset();
        Admitted.reset();
        Selected = nullptr;
        SelectedRemote = {};
        Cadence = {};
        for (auto& slot : Slots) slot.Result.reset();
    }

    void ResetRemote()
    {
        if (!Bank) return;
        RemoteEpoch = Runtime->NewEpoch();
        Require(Bank->Reset(RemoteEpoch), "Cloud snapshot bank has an unretired preparation");
        BankNeedsLayout = true;
        Pending = {};
        Imported = {};
        SelectedRemote = {};
    }

    void RecordDisplay(const Sample& result, bool repeat)
    {
        const auto age = result.Inputs.MeasureAge(Input::Clock::now(), Settings.MaxAgeMs);
        if (age.OverAge && (!repeat || !Stats.DisplayAge.OverAge)) ++Stats.OverAgeCrossings;
        Stats.Effective = Active;
        Stats.DisplayedInvocation = result.Invocation;
        Stats.DisplayAge = age;
        if (repeat) ++Stats.Repeats;
    }

    GDX12CommandQueue& Direct() const { return *Primary.Device->GetCommandQueue(); }
    Placement DirectPlacement() const { return { Primary.Device->GetSubmissionAdapterId(), QueueKind::Direct }; }
    Placement ComputePlacement() const { return { Primary.Device->GetSubmissionAdapterId(), QueueKind::Compute }; }
    Placement SecondaryPlacement() const { return { Secondary->GetSubmissionAdapterId(), QueueKind::Compute }; }
    Placement ExportPlacement() const { return { SecondaryPlacement().Adapter, QueueKind::Copy }; }
    Placement ImportPlacement() const { return { DirectPlacement().Adapter, QueueKind::Copy }; }

    Slot* FreeSlot()
    {
        if (!Reached(RecorderReady, *Compute)) return nullptr;
        auto found = std::find_if(Slots.begin(), Slots.end(), [&](const Slot& slot)
        {
            return &slot != Selected && Reached(slot.Produced, *Compute) && Reached(slot.Read, Direct());
        });
        if (found == Slots.end()) return nullptr;
        if (found->CubeSize != Layout->CubeSize())
        {
            auto buffer = std::make_shared<GDX12Resource>();
            buffer->D3DResource = Primary.Device->CreateResource(D3D12_HEAP_TYPE_DEFAULT,
                Layout->BufferDesc(), D3D12_RESOURCE_STATE_COMMON);
            found->Payload = { std::move(buffer), NewIdentity() };
            found->CubeSize = Layout->CubeSize();
        }
        if (RecorderSize != Layout->CubeSize())
        {
            Recorder = std::make_shared<GDX12EnvironmentIBLPass::CloudProducer>(Primary, *Layout);
            RecorderSize = Layout->CubeSize();
        }
        return &*found;
    }

    Batch Produce(Graph::Resource payload, const Input& input, TransferVersion version = {})
    {
        const bool remote = version.IsValid();
        auto* recorder = remote ? SecondaryRecorder->Recorder.get() : Recorder.get();
        return Batch("Cloud environment produce", remote ? SecondaryPlacement() : ComputePlacement(),
            { { payload, Graph::Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST, version } },
            [recorder, input, payload](GDX12CommandList* list, const Graph::BoundResources& views)
            {
                recorder->Record(list, input, *views.Get(payload));
            });
    }

    void CommitProducer(Slot& slot, const Input& input, CompletionPoint ready)
    {
        Invocation = NextInvocation();
        slot.Result = Sample{ input, Invocation };
        slot.Produced = RecorderReady = ready;
        slot.Read = {};
        Admitted = input;
        ++Stats.Submitted;
    }

    void PrepareLatest()
    {
        for (auto& slot : Slots)
            if (slot.Result && slot.Result->Inputs.IsCompatibleWith(*Current) &&
                (!Installed || slot.Result->Invocation > Installed->Invocation) &&
                (!Selected || slot.Result->Invocation > Selected->Result->Invocation) &&
                Reached(slot.Produced, *Compute))
                Selected = &slot;
    }

    std::optional<Graph::ExecutionReceipt> SubmitLocalPlan(const Graph& graph, Graph::ExecutionBindings bindings)
    {
        if (Runtime) return Runtime->TrySubmit(graph, std::move(bindings));
        bindings.Queues = { { Primary.Device, &Direct() }, { Primary.Device, Compute } };
        return Executor.TrySubmit(graph, bindings);
    }

    bool ProduceLatest()
    {
        auto* slot = FreeSlot();
        if (!slot) return false;
        NextInvocation();
        Graph graph;
        Graph::ExecutionBindings bindings;
        bindings.KeepAlive = { Recorder };
        const auto payload = Bind(graph, bindings, "Cloud payload", slot->Payload, false);
        auto produce = Produce(payload, *Current);
        graph.AddPass(produce);
        graph.CompilePlan();
        const auto receipt = SubmitLocalPlan(graph, std::move(bindings));
        if (!receipt) return false;
        CommitProducer(*slot, *Current, receipt->Batches.at(0));
        return true;
    }

    std::array<Graph::Resource, 3> BindOutputs(Graph& graph, Graph::ExecutionBindings& bindings,
        GDX12EnvironmentIBLPass& environment, const Graph::LegacyBoundary& boundary)
    {
        if (!boundary.Inputs.empty() || boundary.Outputs.size() != environment.GetNumOutputs())
            throw std::invalid_argument("Cloud split requires all environment output links");
        std::array<Graph::Resource, 3> outputs{};
        for (size_t i = 0; i < outputs.size(); ++i)
        {
            const auto link = environment.GetOutput(CubeOutputs[i]);
            const auto found = std::find_if(boundary.Outputs.begin(), boundary.Outputs.end(),
                [link](const auto& candidate) { return candidate.Link == link; });
            if (found == boundary.Outputs.end())
                throw std::invalid_argument("Cloud split output does not match the environment links");
            if (OutputOwners[i].lock() != found->Native)
            {
                OutputOwners[i] = found->Native;
                OutputIds[i] = NewIdentity();
            }
            outputs[i] = Bind(graph, bindings, "Cloud display cube", { found->Native, OutputIds[i] }, true);
        }
        return outputs;
    }

    Batch Unpack(Graph::Resource payload, const std::array<Graph::Resource, 3>& outputs, TransferVersion version = {})
    {
        std::vector<Graph::Access> accesses{ { payload, Graph::Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE, version } };
        for (const auto output : outputs)
            accesses.push_back({ output, Graph::Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST });
        return Batch("Cloud environment unpack", DirectPlacement(), std::move(accesses),
            [layout = Layout, payload, outputs](GDX12CommandList* list, const Graph::BoundResources& views)
            {
                const std::array<ID3D12Resource*, 3> cubes{ views.Get(outputs[0]), views.Get(outputs[1]), views.Get(outputs[2]) };
                layout->RecordUnpack(list, cubes, *views.Get(payload));
            });
    }

    std::optional<CompletionPoint> SubmitLocal(GDX12EnvironmentIBLPass& environment, const Graph::LegacyBoundary& boundary)
    {
        const bool exact = !UseLatest;
        if (exact && !RequestExact) return {};
        auto* slot = exact ? FreeSlot() : Selected;
        if (!slot)
        {
            if (exact) ++Stats.ExactDeferred;
            return {};
        }
        const Sample sample = exact ? Sample{ *Current, NextInvocation() } : *slot->Result;
        Graph graph;
        Graph::ExecutionBindings bindings;
        const auto payload = Bind(graph, bindings, "Cloud payload", slot->Payload, !exact);
        const auto outputs = BindOutputs(graph, bindings, environment, boundary);
        std::optional<Batch> produce;
        if (exact)
        {
            bindings.KeepAlive.push_back(Recorder);
            produce.emplace(Produce(payload, sample.Inputs));
            graph.AddPass(*produce);
        }
        auto unpack = Unpack(payload, outputs);
        graph.AddPass(unpack);
        graph.CompilePlan();
        const auto receipt = SubmitLocalPlan(graph, std::move(bindings));
        if (!receipt)
        {
            if (exact) ++Stats.ExactDeferred;
            return {};
        }
        if (exact) CommitProducer(*slot, sample.Inputs, receipt->Batches.front());
        slot->Read = receipt->Batches.back();
        environment.PublishCloudSnapshot(sample.Inputs);
        Installed = sample;
        RecordDisplay(sample, false);
        return slot->Read;
    }

    bool PrepareBank()
    {
        if (!Bank)
        {
            Stream = Runtime->NewTransferId();
            RemoteEpoch = Runtime->NewEpoch();
            Bank = Runtime->CreateBank(Stream, RemoteEpoch, SecondaryPlacement().Adapter,
                DirectPlacement().Adapter, Layout->BufferDesc());
        }
        if (BankNeedsLayout && Runtime->TryReconfigureBank(*Bank, Layout->BufferDesc())) BankNeedsLayout = false;
        return !BankNeedsLayout;
    }

    RemoteSource* FreeRemoteSource()
    {
        if (!Reached(SecondaryReady, *Compute)) return nullptr;
        auto slot = std::find_if(RemoteSources.begin(), RemoteSources.end(), [&](const RemoteSource& candidate)
        {
            return !candidate.Ticket.Id() || Bank->SourceReusable(candidate.Ticket);
        });
        if (slot == RemoteSources.end()) return nullptr;
        if (slot->CubeSize != Layout->CubeSize())
        {
            auto buffer = std::make_shared<GDX12Resource>();
            buffer->D3DResource = Secondary->CreateResource(D3D12_HEAP_TYPE_DEFAULT,
                Layout->BufferDesc(), D3D12_RESOURCE_STATE_COMMON);
            slot->Payload = { std::move(buffer), { Runtime->NewResourceId(), SecondaryPlacement().Adapter,
                RemoteEpoch, Stream, Runtime->NewAllocationId() } };
            slot->CubeSize = Layout->CubeSize();
        }
        slot->Payload.Identity.ResourceEpoch = RemoteEpoch;
        if (SecondaryRecorderSize != Layout->CubeSize())
        {
            SecondaryRecorder = std::make_shared<RemoteRecorder>(*Secondary, *Layout);
            SecondaryRecorderSize = Layout->CubeSize();
        }
        return &*slot;
    }

    Batch CopyStage(const bool exporting, const Snapshot::Ticket& ticket,
        Graph::Resource source, Graph::Resource destination, TransferVersion version)
    {
        return Batch(exporting ? "Cloud environment export" : "Cloud environment import",
            exporting ? ExportPlacement() : ImportPlacement(),
            { { source, Graph::Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE, version },
              { destination, Graph::Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST, version } },
            [bank = Bank, exporting, ticket](GDX12CommandList* list, const Graph::BoundResources&)
            {
                auto& native = *list->GetCommandList().Get();
                if (exporting) bank->RecordExportCopies(ticket, native);
                else bank->RecordImportCopies(ticket, native);
            });
    }

    // Cancel only when the runtime proves no commands were issued.
    template<class Submit, class Cancel>
    std::optional<Graph::ExecutionReceipt> SubmitReserved(Submit submit, Cancel cancel)
    {
        std::optional<Graph::ExecutionReceipt> receipt;
        try { receipt = submit(); }
        catch (...)
        {
            if (!Runtime->IsStopped()) cancel();
            throw;
        }
        if (!receipt) cancel();
        return receipt;
    }

    void Cancel(const Snapshot::Ticket& ticket)
    {
        Require(Bank->Cancel(ticket), "Unsubmitted cloud stage could not be cancelled");
    }

    std::optional<Publication> ReserveRemote(RemoteSource& source, SnapshotReadMode mode)
    {
        const auto sequence = NextInvocation();
        const TransferVersion version{ Stream, RemoteEpoch, ProducerInvocationId{ sequence } };
        const auto credit = Bank->Reserve(version, mode, *Compute, source.Payload.Resource->D3DResource,
            source.Payload.Identity.Allocation);
        if (credit.Status == Snapshot::Result::Deferred)
        {
            if (mode == SnapshotReadMode::ExactVersion) Bank->CancelDeferredExact(version);
            return {};
        }
        Require(credit.Status == Snapshot::Result::Accepted, "Cloud producer reservation rejected");
        Invocation = sequence;
        source.Ticket = credit.Publication;
        return Publication{ Sample{ *Current, sequence }, credit, version, source.Payload, {}, {} };
    }

    void CommitRemoteProducer(const Publication& publication, CompletionPoint ready)
    {
        SecondaryReady = ready;
        Admitted = publication.Data.Inputs;
        ++Stats.Submitted;
    }

    bool ProduceRemoteLatest()
    {
        auto* slot = FreeRemoteSource();
        if (!slot) return false;
        auto publication = ReserveRemote(*slot, SnapshotReadMode::LatestCompleted);
        if (!publication) return false;
        const auto ticket = publication->Credit.Publication;
        const auto receipt = SubmitReserved([&]
        {
            Graph graph;
            Graph::ExecutionBindings bindings;
            bindings.KeepAlive = { Bank, SecondaryRecorder };
            const auto payload = Bind(graph, bindings, "Cloud producer payload", publication->Source, false);
            auto produce = Produce(payload, publication->Data.Inputs, publication->Version);
            graph.AddPass(produce);
            graph.CompilePlan();
            return Runtime->TrySubmit(graph, std::move(bindings), [&](const Graph::ExecutionReceipt& issued)
            {
                Require(Bank->BindProducer(ticket, issued.Batches.at(0)), "Cloud producer receipt rejected");
            });
        }, [&] { Cancel(ticket); });
        if (!receipt) return false;
        publication->Produced = receipt->Batches.at(0);
        CommitRemoteProducer(*publication, publication->Produced);
        auto& pending = Pending.at(publication->Credit.SharedSlot);
        Require(!pending, "Cloud shared credit overlaps an unretired publication");
        pending = std::move(publication);
        return true;
    }

    bool ExportRemote(Publication& publication)
    {
        const auto ticket = publication.Credit.Publication;
        const auto ready = Bank->PrepareExport(ticket);
        if (!ready) return true;
        const auto receipt = SubmitReserved([&]
        {
            const auto& shared = Bank->GetSlots().at(publication.Credit.SharedSlot);
            Graph graph;
            Graph::ExecutionBindings bindings;
            const auto source = Bind(graph, bindings, "Completed cloud producer", publication.Source, true);
            const auto target = Bind(graph, bindings, "Cloud shared export", shared.Export, false, true);
            graph.ImportVersion(source, publication.Version);
            bindings.ExactInputs.push_back({ source, publication.Version, *ready, Bank });
            auto exportBatch = CopyStage(true, ticket, source, target, publication.Version);
            graph.AddPass(exportBatch);
            graph.CompilePlan();
            return Runtime->TrySubmit(graph, std::move(bindings), [&](const Graph::ExecutionReceipt& issued)
            {
                const auto point = issued.Batches.at(0);
                Bank->CommitExport(ticket, point, point);
            });
        }, [&] { Cancel(ticket); });
        if (!receipt) return false;
        publication.Exported = receipt->Batches.at(0);
        return true;
    }

    bool ImportRemote(const Publication& publication, GDX12CommandQueue& exportQueue)
    {
        const auto ticket = publication.Credit.Publication;
        const auto import = Bank->PrepareImport(ticket);
        if (import.Status == Snapshot::Result::NotReady || import.Status == Snapshot::Result::Deferred) return false;
        Require(import.Status == Snapshot::Result::Accepted, "Cloud import credit rejected");
        const auto receipt = SubmitReserved([&]
        {
            const auto& shared = Bank->GetSlots().at(import.SharedSlot);
            Graph graph;
            Graph::ExecutionBindings bindings;
            const auto source = Bind(graph, bindings, "Completed cloud export", shared.Import, true, true);
            const auto target = Bind(graph, bindings, "Cloud local import", Bank->GetSlots().at(import.LocalSlot).Local, false);
            graph.ImportVersion(source, publication.Version);
            bindings.ExactInputs.push_back({ source, publication.Version, import.ExportReady, Bank,
                &exportQueue, shared.Export.Identity });
            auto importBatch = CopyStage(false, ticket, source, target, publication.Version);
            graph.AddPass(importBatch);
            graph.CompilePlan();
            return Runtime->TrySubmit(graph, std::move(bindings), [&](const Graph::ExecutionReceipt& issued)
            {
                const auto point = issued.Batches.at(0);
                Bank->CommitImport(ticket, point, point);
            });
        }, [&] { Cancel(ticket); });
        if (receipt) Imported.at(import.LocalSlot) = publication.Data;
        else
        {
            if (publication.Data.Invocation == Invocation) Admitted.reset();
            ++Stats.LatestSkipped;
        }
        return true;
    }

    void PrepareRemoteLatest()
    {
        // Select first so the transport can retire the older displayed slot before admitting imports.
        const auto selected = Bank->SelectLatest(FrameSubmissionId{ FrameSequence }).Current;
        if (selected.IsValid())
        {
            const auto& sample = Imported.at(selected.LocalSlot);
            Require(sample && sample->Invocation == static_cast<uint64_t>(selected.Version.Invocation),
                "Cloud import selected without its matching immutable input");
            if (sample->Inputs.IsCompatibleWith(*Current) && (!Installed || sample->Invocation > Installed->Invocation))
                SelectedRemote = selected;
        }
        auto& exportQueue = *Runtime->GetQueue(ExportPlacement());
        for (auto& publication : Pending)
        {
            if (!publication) continue;
            if (publication->Exported.IsValid())
            {
                if (Reached(publication->Exported, exportQueue) && ImportRemote(*publication, exportQueue))
                    publication.reset();
            }
            else if (Reached(publication->Produced, *Compute) && !ExportRemote(*publication))
            {
                if (publication->Data.Invocation == Invocation) Admitted.reset();
                ++Stats.LatestSkipped;
                publication.reset();
            }
        }
    }

    std::optional<CompletionPoint> SubmitRemoteLatest(GDX12EnvironmentIBLPass& environment,
        const Graph::LegacyBoundary& boundary)
    {
        if (!SelectedRemote.IsValid()) return {};
        const auto reader = Bank->AcquireReader(SelectedRemote.Publication, Direct());
        if (!reader) return {};
        const auto receipt = SubmitReserved([&]
        {
            Graph graph;
            Graph::ExecutionBindings bindings;
            const auto payload = Bind(graph, bindings, "Selected cloud snapshot", reader->View, true);
            const auto version = reader->Lease.View.Version;
            graph.ImportVersion(payload, version);
            bindings.ExactInputs.push_back({ payload, version, reader->Lease.View.ImportReady, Bank });
            auto unpack = Unpack(payload, BindOutputs(graph, bindings, environment, boundary), version);
            graph.AddPass(unpack);
            graph.CompilePlan();
            return Runtime->TrySubmit(graph, std::move(bindings), [&](const Graph::ExecutionReceipt& issued)
            {
                Require(Bank->BindReader(reader->Lease.Id, issued.Batches.at(0)), "Cloud Latest reader receipt rejected");
            });
        }, [&] { Require(Bank->CancelReader(reader->Lease.Id), "Unsubmitted cloud Latest reader could not be cancelled"); });
        if (!receipt) return {};
        Installed = Imported.at(SelectedRemote.LocalSlot);
        environment.PublishCloudSnapshot(Installed->Inputs);
        RecordDisplay(*Installed, false);
        return receipt->Batches.at(0);
    }

    std::optional<CompletionPoint> SubmitRemoteExact(GDX12EnvironmentIBLPass& environment,
        const Graph::LegacyBoundary& boundary)
    {
        if (!RequestExact) return {};
        auto* slot = FreeRemoteSource();
        auto publication = slot ? ReserveRemote(*slot, SnapshotReadMode::ExactVersion) : std::nullopt;
        if (!publication) return {};
        const auto ticket = publication->Credit.Publication;
        const auto version = publication->Version;
        const auto receipt = SubmitReserved([&]() -> std::optional<Graph::ExecutionReceipt>
        {
            Require(Bank->PrepareExact(ticket), "Cloud Exact transaction could not be prepared");
            const auto reader = Bank->PrepareExactReader(ticket, Direct());
            if (!reader) return {};
            const auto& shared = Bank->GetSlots().at(publication->Credit.SharedSlot);
            Graph graph;
            Graph::ExecutionBindings bindings;
            bindings.KeepAlive = { Bank, SecondaryRecorder };
            const auto source = Bind(graph, bindings, "Cloud producer payload", publication->Source, false);
            const auto exported = Bind(graph, bindings, "Cloud shared export", shared.Export, false, true);
            const auto imported = Bind(graph, bindings, "Cloud shared import", shared.Import, false, true);
            const auto local = Bind(graph, bindings, "Cloud local import", reader->View, false);
            auto produce = Produce(source, publication->Data.Inputs, version);
            auto exportBatch = CopyStage(true, ticket, source, exported, version);
            auto importBatch = CopyStage(false, ticket, imported, local, version);
            auto unpack = Unpack(local, BindOutputs(graph, bindings, environment, boundary), version);
            for (auto* batch : { &produce, &exportBatch, &importBatch, &unpack }) graph.AddPass(*batch);
            graph.CompilePlan();
            return Runtime->TrySubmit(graph, std::move(bindings), [&](const Graph::ExecutionReceipt& issued)
            {
                const auto& points = issued.Batches;
                Require(points.size() == 4, "Cloud Exact receipt did not contain four batches");
                Bank->CommitExact(ticket, points[0], points[1], points[1], points[2], points[2]);
                Require(Bank->BindReader(reader->Id, points[3]), "Cloud Exact reader receipt rejected");
                Require(Bank->ReleaseExact(ticket), "Cloud Exact pin was not released");
            });
        }, [&] { Cancel(ticket); });
        if (!receipt) return {};
        CommitRemoteProducer(*publication, receipt->Batches.front());
        Installed = publication->Data;
        environment.PublishCloudSnapshot(Installed->Inputs);
        RecordDisplay(*Installed, false);
        return receipt->Batches.back();
    }
};

CrossAdapterEnvironment::CrossAdapterEnvironment(GDX12EnvironmentIBLPass& environment, GDX12DeviceResources& primary,
    GDX12Device* secondary, RenderMGPURuntime* runtime)
    : _environment(environment), _state(std::make_unique<State>(primary, secondary, runtime))
{
    _flags = environment.GetFlags();
    _numOutputs = environment.GetNumOutputs();
}

CrossAdapterEnvironment::~CrossAdapterEnvironment()
{
    if (_state->Attached) _environment.SetCloudSource(nullptr);
}

GDX12CloudComponent& CrossAdapterEnvironment::CloudSource() { return _state->Cloud; }

const CrossAdapterEnvironment::Diagnostics& CrossAdapterEnvironment::GetDiagnostics() const { return _state->Stats; }

bool CrossAdapterEnvironment::PrepareFrame(const double worldDeltaSeconds)
{
    auto& state = *_state;
    state.Executor.CollectCompleted();
    const bool attach = BistroClouds.GetValue();
    if (attach != state.Attached)
    {
        _environment.SetCloudSource(attach ? &state.Cloud : nullptr);
        state.Attached = attach;
    }
    if (state.Attached) state.Cloud.Advance(worldDeltaSeconds);
    auto input = _environment.CaptureCloudInputs();
    const bool wantsRemote = MultiGPU.GetValue();
    const bool wantsLocal = LocalCompute.GetValue();
    state.Stats.Requested = !input ? Route::Direct :
        (wantsRemote ? Route::SecondaryCompute : (wantsLocal ? Route::LocalCompute : Route::Direct));
    GDX12CommandQueue* compute = nullptr;
    auto active = Route::Direct;
    if (input && wantsRemote && state.Runtime && state.Secondary)
    {
        compute = state.Runtime->GetQueue(state.SecondaryPlacement());
        if (compute) active = Route::SecondaryCompute;
    }
    if (input && !compute && wantsLocal)
    {
        compute = state.Primary.Device->TryGetComputeQueue();
        if (compute) active = Route::LocalCompute;
    }
    const bool latest = Latest.GetValue();
    const bool compatible = input && state.Current && input->IsCompatibleWith(*state.Current);
    if ((state.Active != Route::Direct || active != Route::Direct) &&
        (state.Active != active || state.UseLatest != latest || !compatible))
    {
        if (state.Active == Route::SecondaryCompute || active == Route::SecondaryCompute) state.ResetRemote();
        state.Reset();
    }
    state.Compute = compute;
    state.Active = active;
    state.UseLatest = latest;
    state.Current = std::move(input);
    state.Selected = nullptr;
    state.SelectedRemote = {};
    state.RequestExact = false;
    state.Settings = GetCloudPublicationSettings();
    if (state.Active == Route::Direct) return false;
    const auto& current = *state.Current;
    if (!state.Layout || state.Layout->CubeSize() != current.IBL.CubeResolution)
        state.Layout = std::make_shared<GDX12CloudEnvironmentLayout>(*state.Primary.Device->GetDevice().Get(), current.IBL.CubeResolution);
    const bool remote = state.Active == Route::SecondaryCompute;
    if (remote && !state.PrepareBank()) return true;
    if (state.UseLatest)
    {
        if (remote)
        {
            if (state.FrameSequence == UINT64_MAX) throw std::overflow_error("Cloud render frame sequence exhausted");
            ++state.FrameSequence;
            state.PrepareRemoteLatest();
        }
        else state.PrepareLatest();
    }
    if (!state.Admitted || current.Cloud.LightingRevision != state.Admitted->Cloud.LightingRevision)
    {
        const auto nowSeconds = std::chrono::duration<double>(current.CapturedAt - state.Origin).count();
        if (state.Cadence.TakeTick(nowSeconds, state.Settings.UpdateHz))
        {
            if (state.UseLatest)
            {
                if (!(remote ? state.ProduceRemoteLatest() : state.ProduceLatest())) ++state.Stats.LatestSkipped;
            }
            else
                state.RequestExact = true;
        }
    }
    return true;
}

void CrossAdapterEnvironment::RecordRenderGraph(GDX12RenderGraphBuilder& graph) { _environment.RecordRenderGraph(graph); }

IRenderPassLink* CrossAdapterEnvironment::GetOutput(const UINT index) { return _environment.GetOutput(index); }

void CrossAdapterEnvironment::Execute(GDX12CommandList* commands)
{
    auto& state = *_state;
    if (state.Active != Route::Direct && state.Installed && (state.UseLatest || !state.RequestExact))
    {
        _environment.RepeatCloudSnapshot(commands, state.Installed->Inputs);
        state.RecordDisplay(*state.Installed, true);
        return;
    }
    state.Stats.Effective = Route::Direct;
    state.Stats.DisplayAge = {};
    state.Stats.DisplayedInvocation = 0;
    state.Installed.reset();
    _environment.Execute(commands);
}

void CrossAdapterEnvironment::Resize()
{
    _environment.Resize();
}

void CrossAdapterEnvironment::ClearDenendencies()
{
    _state->Executor.Drain();
    if (_state->Attached)
    {
        _environment.SetCloudSource(nullptr);
        _state->Attached = false;
    }
    _state->Cloud.ClearDenendencies();
    _environment.ClearDenendencies();
}

std::optional<CompletionPoint> CrossAdapterEnvironment::Submit(const Graph::LegacyBoundary& boundary, CompletionPoint prefix)
{
    auto& state = *_state;
    if (boundary.Replaced != this || state.Active == Route::Direct || !_environment.IsCloudDisplayPrepared(*state.Layout)) return {};
    if (!prefix.IsValid() || !(prefix.Timeline() == state.Direct().GetTimelineIdentity()))
        throw std::invalid_argument("Cloud replacement requires the primary DIRECT prefix");
    if (state.Active == Route::LocalCompute) return state.SubmitLocal(_environment, boundary);
    if (state.BankNeedsLayout) return {};
    if (state.UseLatest) return state.SubmitRemoteLatest(_environment, boundary);
    const auto receipt = state.SubmitRemoteExact(_environment, boundary);
    if (!receipt && state.RequestExact) ++state.Stats.ExactDeferred;
    return receipt;
}

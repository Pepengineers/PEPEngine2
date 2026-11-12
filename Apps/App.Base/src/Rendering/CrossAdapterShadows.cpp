#include "App.Base/Rendering/CrossAdapterShadows.h"
#include "Common/ConsoleVariables.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12CommandQueue.h"
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace
{
    using Graph = GDX12RenderGraph;
    using namespace GDX12Snapshot;
    using namespace GDX12Submission;

    AutoConsoleVariable<bool> Enabled(L"Render.Shadows.MultiGPU", false, L"Render selected shadows on the secondary adapter");
    AutoConsoleVariable<int> SelectionMode(L"Render.Shadows.MultiGPU.Selection", 0,
        L"0 all, 1 sun, 2 all local, 3 specific local light", EConsoleVariableFlags::Default,
        [](int value) { return value >= 0 && value <= 3; });
    AutoConsoleVariable<std::int64_t> EntitySlot(L"Render.Shadows.MultiGPU.Entity", 0,
        L"ECS slot to resolve when selecting a specific local light", EConsoleVariableFlags::Default,
        [](std::int64_t value) { return value >= 0 && value <= UINT_MAX; });

    void Require(bool condition, const char* message)
    {
        if (!condition) throw std::logic_error(message);
    }

    bool SameIdentity(const ResourceIdentity& a, const ResourceIdentity& b)
    {
        return a.Id == b.Id && a.Adapter == b.Adapter && a.ResourceEpoch == b.ResourceEpoch &&
            a.Transfer == b.Transfer && a.Allocation == b.Allocation;
    }

    class Stage final : public GDX12RenderPass
    {
    public:
        using Recorder = std::function<void(GDX12CommandList&, const Graph::BoundResources&)>;
        Stage(const char* name, Placement placement, std::vector<Graph::Access> accesses, Recorder recorder)
            : _name(name), _placement(placement), _accesses(std::move(accesses)), _record(std::move(recorder)) {}

        void RecordRenderGraph(GDX12RenderGraphBuilder& graph) override
        {
            graph.SetName(_name);
            graph.SetPlacement(_placement);
            graph.DeclareCompleteAccesses();
            graph.HasSideEffects();
            for (const auto& access : _accesses)
                if (access.Version.IsValid()) graph.UseVersion(access.Id, access.Usage, access.Version, access.State);
                else graph.Use(access.Id, access.Usage, access.State);
        }
        bool SupportsGraphQueue(QueueKind queue) const override { return queue == _placement.Queue; }
        void RecordPlacedCommands(GDX12CommandList* list, const Graph::BoundResources& views) override
        { _record(*list, views); }
    private:
        const char* _name;
        Placement _placement;
        std::vector<Graph::Access> _accesses;
        Recorder _record;
    };

    class Plan
    {
    public:
        Graph::Resource Bind(const char* name, const PhysicalView& view, bool imported = true, bool shared = false)
        {
            Require(view.Resource && view.Resource->D3DResource, "Missing placed shadow resource");
            auto* native = view.Resource->D3DResource.Get();
            const auto physical = ViewByNative.find(native);
            if (const auto known = ViewById.find(view.Identity.Id); known != ViewById.end())
            {
                const auto& existing = Bindings.Views[known->second];
                Require(physical != ViewByNative.end() && physical->second == known->second &&
                    existing.Native == view.Resource && SameIdentity(existing.Identity, view.Identity),
                    "Placed shadow view has conflicting canonical identities");
                return existing.View;
            }
            Require(physical == ViewByNative.end(), "Placed shadow view has conflicting canonical identities");
            const auto id = imported ? GraphPlan.Import(name) : GraphPlan.Create(name);
            GraphPlan.RegisterResource(id, view.Identity, shared);
            ViewById.emplace(view.Identity.Id, Bindings.Views.size());
            ViewByNative.emplace(native, Bindings.Views.size());
            Bindings.Views.push_back({ id, view.Identity, view.Resource });
            return id;
        }

        Graph::Pass Add(const char* name, Placement location, std::vector<Graph::Access> accesses, Stage::Recorder record)
        {
            Passes.push_back(std::make_unique<Stage>(name, location, std::move(accesses), std::move(record)));
            return GraphPlan.AddPass(*Passes.back());
        }

        std::optional<Graph::Pass> Raster(const CrossAdapterShadows::RasterPlan& raster, Placement location)
        {
            Bindings.KeepAlive.insert(Bindings.KeepAlive.end(), raster.Owners.begin(), raster.Owners.end());
            std::optional<Graph::Pass> previous;
            for (const auto& stage : raster.Stages)
            {
                std::vector<Graph::Access> accesses;
                std::vector<std::pair<Graph::Resource, GDX12ResourceHandle>> handles;
                Require(static_cast<bool>(stage.Record), "Shadow raster stage has no recorder");
                for (const auto& use : stage.Accesses)
                {
                    const auto id = Bind(stage.Name, use.View);
                    accesses.push_back({ id, use.Mode, use.State, use.Version });
                    handles.emplace_back(id, use.View.Resource->D3DResource);
                }
                const auto pass = Add(stage.Name, location, std::move(accesses),
                    [record = stage.Record, handles = std::move(handles)](GDX12CommandList& list, const Graph::BoundResources& views)
                    {
                        for (const auto& [id, handle] : handles)
                            Require(views.Get(id) == handle.Get(), "Shadow raster bindings changed before recording");
                        record(list);
                    });
                if (previous) GraphPlan.DependsOn(pass, *previous);
                previous = pass;
            }
            for (const auto& input : raster.ExactInputs)
            {
                const bool read = std::any_of(raster.Stages.begin(), raster.Stages.end(), [&](const auto& stage)
                {
                    return std::any_of(stage.Accesses.begin(), stage.Accesses.end(), [&](const auto& access)
                    {
                        return access.Mode != Graph::Mode::Write && access.View.Identity.Id == input.View.Identity.Id &&
                            access.Version == input.Version;
                    });
                });
                if (!read) continue;
                const auto id = Bind("Shadow exact input", input.View);
                const auto existing = std::find_if(Bindings.ExactInputs.begin(), Bindings.ExactInputs.end(),
                    [id, &input](const auto& binding) { return binding.View == id && binding.Version == input.Version; });
                if (existing != Bindings.ExactInputs.end())
                {
                    Require(existing->Ready.Timeline() == input.Ready.Timeline() && existing->Ready.Value() == input.Ready.Value(),
                        "Conflicting issued points for the same shadow input");
                    continue;
                }
                GraphPlan.ImportVersion(id, input.Version);
                Bindings.ExactInputs.push_back({ id, input.Version, input.Ready, input.Owner });
            }
            return previous;
        }

        size_t BatchIndex(Graph::Pass pass) const
        {
            const auto& batches = GraphPlan.GetCompiledPlan().Batches;
            const auto it = std::find_if(batches.begin(), batches.end(), [pass](const auto& batch)
                { return batch.Passes.size() == 1 && batch.Passes.front() == pass; });
            Require(it != batches.end(), "Missing shadow stage receipt index");
            return static_cast<size_t>(it - batches.begin());
        }

        std::vector<std::unique_ptr<Stage>> Passes;
        GDX12RenderGraph GraphPlan;
        Graph::ExecutionBindings Bindings;
        std::unordered_map<ResourceId, size_t> ViewById;
        std::unordered_map<ID3D12Resource*, size_t> ViewByNative;
    };
}

struct CrossAdapterShadows::Stream
{
    struct Sample
    {
        GDX12ShadowTransferLayout Layout;
        double SourceTime;
        TransferVersion Version;
    };
    struct Frame
    {
        PhysicalView Payload;
        std::shared_ptr<GDX12Resource> PrimaryOwner;
        CompletionPoint Tail;
    };
    struct Publication
    {
        Reservation Credit;
        std::shared_ptr<Frame> Source;
        std::shared_ptr<const Sample> Data;
        CompletionPoint Produced, Exported;
    };

    TransferId Id = TransferId::Invalid;
    Epoch LayoutEpoch = Epoch::Invalid;
    UINT Layers = 0;
    UINT Extent = 0;
    bool Configured = false;
    std::shared_ptr<Transport> Bank;
    std::array<std::shared_ptr<Frame>, SlotCount> Frames;
    Frame* AwaitingTail = nullptr;
    std::weak_ptr<GDX12Resource> InitializedAtlas;
    ID3D12Resource* InitializedNative = nullptr;
    ResourceId InitializedAtlasId = ResourceId::Invalid;
    GDX12ShadowDepthTransfer::InitializedSlices InitializedSlices;
    std::array<std::optional<Publication>, SlotCount> Pending;
    std::array<std::shared_ptr<const Sample>, SlotCount> Imported;
    std::shared_ptr<const Sample> Displayed;
    std::vector<GDX12ShadowOutput> DisplayedOutputs;
    uint64_t FrameSequence = 0;
    Statistics Stats;
};

bool IsMultiGPUShadowsEnabled() { return Enabled.GetValue() || IsShadowPolicyEnabled(); }

CrossAdapterShadows::CrossAdapterShadows(GDX12Device& primary, GDX12Device& secondary, RenderMGPURuntime& runtime)
    : _primary(primary), _secondary(secondary), _runtime(runtime) {}

CrossAdapterShadows::~CrossAdapterShadows() = default;

GDX12ShadowSelection CrossAdapterShadows::ReadSelection(const std::vector<GDX12ShadowOutput>& preparedLocal)
{
    using Mode = GDX12ShadowSelection::Mode;
    if (IsShadowPolicyEnabled())
    {
        GDX12ShadowSelection result;
        result.Scope = Mode::Set;
        result.SunSelected = ReadShadowUpdatePolicy().Secondary;
        for (const auto& output : preparedLocal)
            if (ReadShadowUpdatePolicy(output.Light.EntityId).Secondary) result.Lights.push_back(output.Light);
        return result;
    }
    const int mode = SelectionMode.GetValue();
    const auto slot = EntitySlot.GetValue();
    if (mode != _chosenMode || slot != _chosenSlot)
    {
        _chosenMode = mode;
        _chosenSlot = slot;
        _chosenLight = {};
        if (mode == static_cast<int>(Mode::SpecificLight))
        {
            const auto it = std::find_if(preparedLocal.begin(), preparedLocal.end(), [slot](const auto& light)
                { return light.Light.Generation != 0 && light.Light.EntityId == slot; });
            if (it != preparedLocal.end()) _chosenLight = it->Light;
        }
    }
    return { static_cast<Mode>(mode), _chosenLight };
}

std::optional<CrossAdapterShadows::Selection> CrossAdapterShadows::Select(GDX12ShadowAtlas atlas,
    const GDX12ShadowSelection& selection, const std::vector<GDX12ShadowOutput>& prepared, const std::vector<bool>& replicaReady)
{
    if (prepared.size() != replicaReady.size() || prepared.size() > UINT_MAX) return std::nullopt;
    std::vector<UINT> admitted, primary;
    std::vector<GDX12ShadowOutput> secondary;
    UINT layer = 0;
    for (UINT i = 0; i < prepared.size(); ++i)
    {
        const auto& group = prepared[i];
        const bool selected = atlas == GDX12ShadowAtlas::Sun ? selection.IncludesSun() : selection.IncludesLocal(group.Light);
        if (!selected || !replicaReady[i])
        {
            primary.push_back(i);
            continue;
        }
        admitted.push_back(i);
        secondary.push_back(group);
        if (atlas == GDX12ShadowAtlas::Local)
        {
            if (layer > D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION - group.ViewCount()) return std::nullopt;
            for (UINT face = 0; face < group.ViewCount(); ++face)
                secondary.back().Views[face].Region.Slice = layer++;
        }
    }
    auto layout = GDX12ShadowTransferLayout::Build(atlas, secondary);
    if (!layout || !layout->ResolveDestination(prepared)) return std::nullopt;
    return Selection{ std::move(*layout), std::move(admitted), std::move(primary), std::move(secondary) };
}

std::optional<CompletionPoint> CrossAdapterShadows::Submit(const Selection& selection,
    const std::vector<GDX12ShadowOutput>& primaryPrepared, const PhysicalView& primaryAtlas,
    const PhysicalView& secondaryAtlas, const RasterPlan& primary, const RasterPlan& secondary, CompletionPoint prefix,
    bool latest, bool update, double sourceTime)
{
    if (!IsMultiGPUShadowsEnabled() && !IsShadowPolicyEnabled()) return std::nullopt;
    const auto atlasIndex = selection.Layout.Atlas() == GDX12ShadowAtlas::Sun ? 0 : 1;
    if (sourceTime == 0) sourceTime = ShadowSourceTimeSeconds();
    if (latest && _streams[atlasIndex] && _streams[atlasIndex]->Displayed)
        return SubmitLatest(selection, primaryPrepared, primaryAtlas, secondaryAtlas, primary, secondary, prefix, update, sourceTime);
    if (!update && _streams[atlasIndex] && _streams[atlasIndex]->Displayed) return prefix;
    Require(!_runtime.IsStopped(), "Shadow submit domain is stopped");
    const auto& layout = selection.Layout;
    const auto destination = layout.ResolveDestination(primaryPrepared);
    if (!destination) return std::nullopt;
    const Placement directA{ _primary.GetSubmissionAdapterId(), QueueKind::Direct };
    const Placement directB{ _secondary.GetSubmissionAdapterId(), QueueKind::Direct };
    auto* direct = _runtime.GetQueue(directA);
    auto* producer = _runtime.GetQueue(directB);
    Require(direct && producer && prefix.IsValid() && prefix.Timeline() == direct->GetTimelineIdentity() &&
        prefix.Value() == direct->GetLastCompletionPoint().Value(), "Shadow restore needs the current primary DIRECT prefix");
    Require(primaryAtlas.Resource && secondaryAtlas.Resource, "Shadow raster atlases must be retained");
    auto& owner = _streams[layout.Atlas() == GDX12ShadowAtlas::Sun ? 0 : 1];
    if (!owner) owner = std::make_unique<Stream>();
    auto& stream = *owner;
    Require(!stream.AwaitingTail, "Previous shadow frame has no tail receipt");
    const auto progress = direct->GetCompletionProgress();
    const auto available = [&](const auto& frame)
        { return !frame || (frame.use_count() == 1 && (!frame->Tail.IsValid() || frame->Tail.IsReachedBy(progress))); };
    Reservation credit;
    TransferVersion version;
    const auto cancel = [&]
    {
        if (credit.Status == Result::Accepted)
            Require(stream.Bank->Cancel(credit.Publication), "Unsubmitted shadow reservation failed to cancel");
        else if (credit.Status == Result::Deferred)
            stream.Bank->CancelDeferredExact(version);
        credit = {};
    };
    bool submitted = false;
    try
    {
        const auto payloadDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32_FLOAT, layout.Extent(), layout.Extent(),
            static_cast<UINT16>(layout.Layers()), 1);
        if (stream.Layers != layout.Layers() || stream.Extent != layout.Extent())
        {
            if (!std::all_of(stream.Frames.begin(), stream.Frames.end(), available)) return std::nullopt;
            const auto epoch = _runtime.NewEpoch();
            if (stream.Bank && !stream.Bank->Reset(epoch)) return std::nullopt;
            stream.Frames = {};
            stream.Layers = layout.Layers();
            stream.Extent = layout.Extent();
            stream.LayoutEpoch = epoch;
            stream.Configured = false;
        }
        if (!stream.Bank)
        {
            if (stream.Id == TransferId::Invalid) stream.Id = _runtime.NewTransferId();
            stream.Bank = _runtime.CreateBank(stream.Id, stream.LayoutEpoch, directB.Adapter, directA.Adapter, payloadDesc);
            stream.Configured = true;
        }
        if (!stream.Configured)
        {
            if (!_runtime.TryReconfigureBank(*stream.Bank, payloadDesc)) return std::nullopt;
            stream.Configured = true;
        }
        auto slot = std::find_if(stream.Frames.begin(), stream.Frames.end(), available);
        if (slot == stream.Frames.end()) return std::nullopt;
        if (!*slot)
        {
            auto desc = payloadDesc;
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            auto frame = std::make_shared<Stream::Frame>();
            auto texture = std::make_shared<GDX12Resource>();
            texture->D3DResource = _secondary.CreateResource(D3D12_HEAP_TYPE_DEFAULT, desc, D3D12_RESOURCE_STATE_COMMON);
            frame->Payload = { std::move(texture), { _runtime.NewResourceId(), directB.Adapter, stream.LayoutEpoch,
                stream.Id, _runtime.NewAllocationId() } };
            *slot = std::move(frame);
        }
        auto& frame = **slot;
        frame.PrimaryOwner.reset();
        frame.Tail = {};

        Require(_invocation < (std::numeric_limits<std::uint64_t>::max)(), "Shadow invocation ID exhausted");
        version = { stream.Id, stream.LayoutEpoch, ProducerInvocationId{ ++_invocation } };
        auto& bank = *stream.Bank;
        credit = bank.Reserve(version, SnapshotReadMode::ExactVersion, *producer,
            frame.Payload.Resource->D3DResource, frame.Payload.Identity.Allocation);
        if (credit.Status != Result::Accepted || !bank.PrepareExact(credit.Publication))
        {
            cancel();
            return std::nullopt;
        }
        const auto reader = bank.PrepareExactReader(credit.Publication, *direct);
        if (!reader)
        {
            cancel();
            return std::nullopt;
        }
        if (!_pack) _pack = std::make_shared<GDX12ShadowDepthTransfer>(_secondary);
        if (!_restore) _restore = std::make_shared<GDX12ShadowDepthTransfer>(_primary);
        auto pack = _pack->PreparePack(layout, secondaryAtlas.Resource, frame.Payload.Resource);
        auto initialized = stream.InitializedAtlasId == primaryAtlas.Identity.Id &&
            stream.InitializedAtlas.lock() == primaryAtlas.Resource &&
            stream.InitializedNative == primaryAtlas.Resource->D3DResource.Get() ?
            stream.InitializedSlices : GDX12ShadowDepthTransfer::InitializedSlices{};
        auto restore = _restore->PrepareRestore(layout, primaryPrepared, primaryAtlas.Resource, reader->View.Resource, initialized);
        if (!restore)
        {
            cancel();
            return std::nullopt;
        }
        for (const auto& view : *destination) initialized.set(view.Region.Slice);

        Plan plan;
        plan.Bindings.KeepAlive = { *slot, stream.Bank, _pack, _restore, pack, restore };
        const auto primaryDepth = plan.Bind("Primary shadow atlas", primaryAtlas);
        const auto secondaryDepth = plan.Bind("Secondary shadow atlas", secondaryAtlas);
        const auto packed = plan.Bind("Packed shadow depth", frame.Payload, false);
        const auto& shared = bank.GetSlots().at(credit.SharedSlot);
        const auto exported = plan.Bind("Shadow shared export", shared.Export, false, true);
        const auto imported = plan.Bind("Shadow shared import", shared.Import, false, true);
        const auto local = plan.Bind("Shadow local import", reader->View, false);
        const auto primaryTail = plan.Raster(primary, directA);
        const auto secondaryTail = plan.Raster(secondary, directB);
        const auto packPass = plan.Add("Shadow pack", directB, {
                { secondaryDepth, Graph::Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
                { packed, Graph::Mode::Write, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, version } },
            [&, secondaryDepth, packed](GDX12CommandList& commands, const Graph::BoundResources& views)
                { _pack->RecordPack(commands, *pack, views.Get(secondaryDepth), views.Get(packed)); });
        if (secondaryTail) plan.GraphPlan.DependsOn(packPass, *secondaryTail);
        const auto exportPass = plan.Add("Shadow export", { directB.Adapter, QueueKind::Copy }, {
                { packed, Graph::Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE, version },
                { exported, Graph::Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST, version } },
            [&](GDX12CommandList& commands, const Graph::BoundResources&)
                { bank.RecordExportCopies(credit.Publication, *commands.GetCommandList().Get()); });
        const auto importPass = plan.Add("Shadow import", { directA.Adapter, QueueKind::Copy }, {
                { imported, Graph::Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE, version },
                { local, Graph::Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST, version } },
            [&](GDX12CommandList& commands, const Graph::BoundResources&)
                { bank.RecordImportCopies(credit.Publication, *commands.GetCommandList().Get()); });
        const auto restorePass = plan.Add("Shadow restore", directA, {
                { local, Graph::Mode::Read, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, version },
                { primaryDepth, Graph::Mode::Write, D3D12_RESOURCE_STATE_DEPTH_WRITE } },
            [&, primaryDepth, local](GDX12CommandList& commands, const Graph::BoundResources& views)
                { _restore->RecordRestore(commands, *restore, views.Get(primaryDepth), views.Get(local)); });
        if (primaryTail) plan.GraphPlan.DependsOn(restorePass, *primaryTail);
        plan.GraphPlan.CompilePlan();
        const size_t packBatch = plan.BatchIndex(packPass), exportBatch = plan.BatchIndex(exportPass);
        const size_t importBatch = plan.BatchIndex(importPass), restoreBatch = plan.BatchIndex(restorePass);
        const auto receipt = _runtime.TrySubmit(plan.GraphPlan, std::move(plan.Bindings), [&](const Graph::ExecutionReceipt& result)
        {
            const auto& points = result.Batches;
            bank.CommitExact(credit.Publication, points.at(packBatch), points.at(exportBatch), points.at(exportBatch),
                points.at(importBatch), points.at(importBatch));
            Require(bank.BindReader(reader->Id, points.at(restoreBatch)), "Shadow restore reader receipt rejected");
            Require(bank.ReleaseExact(credit.Publication), "Shadow exact publication pin release failed");
            if (secondary.Commit) secondary.Commit();
            if (primary.Commit) primary.Commit();
            stream.InitializedAtlas = primaryAtlas.Resource;
            stream.InitializedNative = primaryAtlas.Resource->D3DResource.Get();
            stream.InitializedAtlasId = primaryAtlas.Identity.Id;
            stream.InitializedSlices = initialized;
            auto sample = std::make_shared<Stream::Sample>(Stream::Sample{ layout, sourceTime, version });
            stream.Displayed = sample;
            stream.DisplayedOutputs = *layout.ResolveSnapshot(primaryPrepared);
            ++stream.Stats.Produced;
            ++stream.Stats.ExactJoins;
            const auto bytes = uint64_t(layout.Extent()) * layout.Extent() * layout.Layers() * sizeof(float);
            stream.Stats.ExportBytes += bytes;
            stream.Stats.ImportBytes += bytes;
            for (size_t i = 0; i < bank.GetSlots().size(); ++i)
                if (bank.GetSlots()[i].Local.Identity.Id == reader->View.Identity.Id) stream.Imported[i] = sample;
        });
        if (!receipt)
        {
            cancel();
            return std::nullopt;
        }
        submitted = true;
        frame.PrimaryOwner = primaryAtlas.Resource;
        frame.Tail = receipt->Batches.at(restoreBatch);
        stream.AwaitingTail = &frame;
        return frame.Tail;
    }
    catch (...)
    {
        if (submitted || _runtime.IsStopped()) throw;
        cancel();
        try { throw; }
        catch (const DxException& error) { OutputDebugStringW(error.ToString().c_str()); }
        catch (const std::exception& error) { OutputDebugStringA(error.what()); }
        catch (...) { OutputDebugStringA("Unknown MGPU shadow pre-submit failure"); }
        return std::nullopt;
    }
}


std::optional<CompletionPoint> CrossAdapterShadows::SubmitPrimary(const RasterPlan& raster, CompletionPoint prefix)
{
    if (raster.Stages.empty()) { if (raster.Commit) raster.Commit(); return prefix; }
    Plan plan;
    plan.Raster(raster, { _primary.GetSubmissionAdapterId(), QueueKind::Direct });
    plan.GraphPlan.CompilePlan();
    const auto receipt = _runtime.TrySubmit(plan.GraphPlan, std::move(plan.Bindings), [&](const auto&)
    { if (raster.Commit) raster.Commit(); });
    return receipt ? std::optional<CompletionPoint>(receipt->Batches.back()) : std::nullopt;
}

const std::vector<GDX12ShadowOutput>& CrossAdapterShadows::GetDisplayedOutputs(GDX12ShadowAtlas atlas) const
{
    static const std::vector<GDX12ShadowOutput> empty;
    const auto& stream = _streams[atlas == GDX12ShadowAtlas::Sun ? 0 : 1];
    return stream ? stream->DisplayedOutputs : empty;
}

double CrossAdapterShadows::GetDisplayedSourceTime(GDX12ShadowAtlas atlas) const
{
    const auto& stream = _streams[atlas == GDX12ShadowAtlas::Sun ? 0 : 1];
    return stream && stream->Displayed ? stream->Displayed->SourceTime : 0;
}

CrossAdapterShadows::Statistics CrossAdapterShadows::GetStatistics(GDX12ShadowAtlas atlas) const
{
    const auto& stream = _streams[atlas == GDX12ShadowAtlas::Sun ? 0 : 1];
    if (!stream) return {};
    auto result = stream->Stats;
    for (const auto& pending : stream->Pending) result.Pending += pending.has_value();
    return result;
}

void CrossAdapterShadows::AdvanceLatest(Stream& stream)
{
    auto& bank = *stream.Bank;
    const Placement produce{ _secondary.GetSubmissionAdapterId(), QueueKind::Direct };
    const Placement exportLocation{ produce.Adapter, QueueKind::Copy };
    const Placement importLocation{ _primary.GetSubmissionAdapterId(), QueueKind::Copy };
    auto& exportQueue = *_runtime.GetQueue(exportLocation);
    const auto reached = [&](CompletionPoint point, Placement placement)
    { return point.IsValid() && point.IsReachedBy(_runtime.GetQueue(placement)->GetCompletionProgress()); };
    for (auto& pending : stream.Pending)
    {
        if (!pending) continue;
        auto& publication = *pending;
        const auto ticket = publication.Credit.Publication;
        const auto version = publication.Data->Version;
        try
        {
            if (!publication.Exported.IsValid())
            {
                if (!reached(publication.Produced, produce)) continue;
                const auto ready = bank.PrepareExport(ticket);
                if (!ready) continue;
                const auto& shared = bank.GetSlots().at(publication.Credit.SharedSlot);
                Plan plan;
                plan.Bindings.KeepAlive = { stream.Bank, publication.Source, publication.Data };
                const auto source = plan.Bind("Completed shadow depth", publication.Source->Payload);
                const auto target = plan.Bind("Shadow Latest export", shared.Export, false, true);
                plan.GraphPlan.ImportVersion(source, version);
                plan.Bindings.ExactInputs.push_back({ source, version, *ready, stream.Bank });
                plan.Add("Shadow Latest export", exportLocation, {
                    { source, Graph::Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE, version },
                    { target, Graph::Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST, version } },
                    [&, ticket](GDX12CommandList& list, const Graph::BoundResources&)
                    { bank.RecordExportCopies(ticket, *list.GetCommandList().Get()); });
                plan.GraphPlan.CompilePlan();
                const auto receipt = _runtime.TrySubmit(plan.GraphPlan, std::move(plan.Bindings), [&](const auto& result)
                { bank.CommitExport(ticket, result.Batches.at(0), result.Batches.at(0)); });
                if (!receipt)
                {
                    Require(bank.Cancel(ticket), "Unsubmitted shadow export cancellation failed");
                    pending.reset();
                    continue;
                }
                publication.Exported = receipt->Batches.at(0);
                stream.Stats.ExportBytes += uint64_t(publication.Data->Layout.Extent()) * publication.Data->Layout.Extent() *
                    publication.Data->Layout.Layers() * sizeof(float);
            }
            if (!reached(publication.Exported, exportLocation)) continue;
            const auto imported = bank.PrepareImport(ticket);
            if (imported.Status == Result::Deferred || imported.Status == Result::NotReady) continue;
            Require(imported.Status == Result::Accepted, "Shadow Latest import was rejected");
            const auto& shared = bank.GetSlots().at(imported.SharedSlot);
            Plan plan;
            plan.Bindings.KeepAlive = { stream.Bank, publication.Source, publication.Data };
            const auto source = plan.Bind("Completed shadow export", shared.Import, true, true);
            const auto target = plan.Bind("Shadow Latest local depth", bank.GetSlots().at(imported.LocalSlot).Local, false);
            plan.GraphPlan.ImportVersion(source, version);
            plan.Bindings.ExactInputs.push_back({ source, version, imported.ExportReady, stream.Bank,
                &exportQueue, shared.Export.Identity });
            plan.Add("Shadow Latest import", importLocation, {
                { source, Graph::Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE, version },
                { target, Graph::Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST, version } },
                [&, ticket](GDX12CommandList& list, const Graph::BoundResources&)
                { bank.RecordImportCopies(ticket, *list.GetCommandList().Get()); });
            plan.GraphPlan.CompilePlan();
            const auto receipt = _runtime.TrySubmit(plan.GraphPlan, std::move(plan.Bindings), [&](const auto& result)
            { bank.CommitImport(ticket, result.Batches.at(0), result.Batches.at(0)); });
            if (receipt)
            {
                stream.Imported.at(imported.LocalSlot) = publication.Data;
                stream.Stats.ImportBytes += uint64_t(publication.Data->Layout.Extent()) * publication.Data->Layout.Extent() *
                    publication.Data->Layout.Layers() * sizeof(float);
            }
            else Require(bank.Cancel(ticket), "Unsubmitted shadow import cancellation failed");
            pending.reset();
        }
        catch (...)
        {
            if (!_runtime.IsStopped()) bank.Cancel(ticket);
            throw;
        }
    }
}

std::optional<CompletionPoint> CrossAdapterShadows::SubmitLatest(const Selection& selection,
    const std::vector<GDX12ShadowOutput>& prepared, const PhysicalView& primaryAtlas,
    const PhysicalView& secondaryAtlas, const RasterPlan& primary, const RasterPlan& secondary,
    CompletionPoint prefix, bool update, double sourceTime)
{
    auto& stream = *_streams[selection.Layout.Atlas() == GDX12ShadowAtlas::Sun ? 0 : 1];
    auto& bank = *stream.Bank;
    const Placement directA{ _primary.GetSubmissionAdapterId(), QueueKind::Direct };
    const Placement directB{ _secondary.GetSubmissionAdapterId(), QueueKind::Direct };
    auto& direct = *_runtime.GetQueue(directA);
    Require(!stream.AwaitingTail, "Previous shadow frame has no tail receipt");
    Require(stream.Extent == selection.Layout.Extent() && stream.Layers == selection.Layout.Layers(),
        "Shadow Latest layout must start a new stream before resizing");
    bank.Poll();
    // Selection retires the older displayed slot before imports need another local credit.
    const auto selected = bank.SelectLatest(FrameSubmissionId{ ++stream.FrameSequence }).Current;
    AdvanceLatest(stream);
    CompletionPoint final = prefix;
    if (!primary.Stages.empty())
    {
        Plan local;
        local.Raster(primary, directA);
        local.GraphPlan.CompilePlan();
        const auto receipt = _runtime.TrySubmit(local.GraphPlan, std::move(local.Bindings), [&](const auto&)
        { if (primary.Commit) primary.Commit(); });
        Require(receipt.has_value(), "Shadow Latest primary work was not submitted");
        final = receipt->Batches.back();
    }
    if (selected.IsValid())
    {
        const auto sample = stream.Imported.at(selected.LocalSlot);
        Require(sample && sample->Version == selected.Version, "Shadow import selected without its metadata");
        if (!stream.Displayed || sample->Version.Invocation > stream.Displayed->Version.Invocation)
        {
            const auto paired = sample->Layout.ResolveSnapshot(prepared);
            Require(paired.has_value(), "Displayed shadow snapshot no longer fits the current light layout");
            const auto reader = bank.AcquireReader(selected.Publication, direct);
            Require(reader.has_value(), "Displayed shadow snapshot has no reader credit");
            bool readerBound = false;
            try
            {
                const auto restore = _restore->PrepareRestore(sample->Layout, *paired, primaryAtlas.Resource,
                    reader->View.Resource, stream.InitializedSlices);
                Require(static_cast<bool>(restore), "Paired shadow restore was rejected");
                Plan plan;
                plan.Bindings.KeepAlive = { stream.Bank, _restore, restore, sample, primaryAtlas.Resource };
                const auto source = plan.Bind("Displayed shadow snapshot", reader->View);
                const auto target = plan.Bind("Primary shadow atlas", primaryAtlas);
                const auto version = reader->Lease.View.Version;
                plan.GraphPlan.ImportVersion(source, version);
                plan.Bindings.ExactInputs.push_back({ source, version, reader->Lease.View.ImportReady, stream.Bank });
                plan.Add("Restore paired Latest shadow", directA, {
                    { source, Graph::Mode::Read, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, version },
                    { target, Graph::Mode::Write, D3D12_RESOURCE_STATE_DEPTH_WRITE } },
                    [&, restore, source, target](GDX12CommandList& list, const Graph::BoundResources& views)
                    { _restore->RecordRestore(list, *restore, views.Get(target), views.Get(source)); });
                plan.GraphPlan.CompilePlan();
                const auto receipt = _runtime.TrySubmit(plan.GraphPlan, std::move(plan.Bindings), [&](const auto& result)
                {
                    Require(bank.BindReader(reader->Lease.Id, result.Batches.at(0)), "Shadow Latest reader receipt rejected");
                    readerBound = true;
                });
                Require(receipt.has_value(), "Paired shadow restore was not submitted");
                final = receipt->Batches.at(0);
                stream.Displayed = sample;
                stream.DisplayedOutputs = *paired;
            }
            catch (...)
            {
                if (!readerBound && !_runtime.IsStopped()) bank.CancelReader(reader->Lease.Id);
                throw;
            }
        }
    }
    if (update)
    {
        const auto slot = std::find_if(stream.Frames.begin(), stream.Frames.end(), [&](const auto& frame)
        {
            return !frame || (frame.use_count() == 1 &&
                (!frame->Tail.IsValid() || frame->Tail.IsReachedBy(direct.GetCompletionProgress())));
        });
        if (slot != stream.Frames.end())
        {
            if (!*slot)
            {
                auto desc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32_FLOAT, stream.Extent, stream.Extent,
                    static_cast<UINT16>(stream.Layers), 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
                auto frame = std::make_shared<Stream::Frame>();
                auto texture = std::make_shared<GDX12Resource>();
                texture->D3DResource = _secondary.CreateResource(D3D12_HEAP_TYPE_DEFAULT, desc, D3D12_RESOURCE_STATE_COMMON);
                frame->Payload = { std::move(texture), { _runtime.NewResourceId(), directB.Adapter, stream.LayoutEpoch,
                    stream.Id, _runtime.NewAllocationId() } };
                *slot = std::move(frame);
            }
            auto& frame = **slot;
            Require(_invocation < (std::numeric_limits<std::uint64_t>::max)(), "Shadow invocation ID exhausted");
            const TransferVersion version{ stream.Id, stream.LayoutEpoch, ProducerInvocationId{ ++_invocation } };
            const auto credit = bank.Reserve(version, SnapshotReadMode::LatestCompleted, *_runtime.GetQueue(directB),
                frame.Payload.Resource->D3DResource, frame.Payload.Identity.Allocation);
            if (credit.Status == Result::Accepted)
            {
                try
                {
                    auto sample = std::make_shared<Stream::Sample>(Stream::Sample{ selection.Layout, sourceTime, version });
                    auto pack = _pack->PreparePack(selection.Layout, secondaryAtlas.Resource, frame.Payload.Resource);
                    Plan plan;
                    plan.Bindings.KeepAlive = { *slot, stream.Bank, _pack, pack, sample };
                    const auto depth = plan.Bind("Secondary shadow atlas", secondaryAtlas);
                    const auto payload = plan.Bind("Shadow Latest producer", frame.Payload, false);
                    const auto raster = plan.Raster(secondary, directB);
                    const auto packed = plan.Add("Pack Latest shadow", directB, {
                        { depth, Graph::Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
                        { payload, Graph::Mode::Write, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, version } },
                        [&, pack, depth, payload](GDX12CommandList& list, const Graph::BoundResources& views)
                        { _pack->RecordPack(list, *pack, views.Get(depth), views.Get(payload)); });
                    if (raster) plan.GraphPlan.DependsOn(packed, *raster);
                    plan.GraphPlan.CompilePlan();
                    const auto batch = plan.BatchIndex(packed);
                    const auto receipt = _runtime.TrySubmit(plan.GraphPlan, std::move(plan.Bindings), [&](const auto& result)
                    {
                        Require(bank.BindProducer(credit.Publication, result.Batches.at(batch)), "Shadow Latest producer rejected");
                        if (secondary.Commit) secondary.Commit();
                    });
                    if (receipt)
                    {
                        auto& pending = stream.Pending.at(credit.SharedSlot);
                        Require(!pending, "Shadow Latest publication overlaps an outstanding slot");
                        pending = Stream::Publication{ credit, *slot, sample, receipt->Batches.at(batch), {} };
                        ++stream.Stats.Produced;
                    }
                    else Require(bank.Cancel(credit.Publication), "Unsubmitted shadow producer cancellation failed");
                }
                catch (...)
                {
                    if (!_runtime.IsStopped()) bank.Cancel(credit.Publication);
                    throw;
                }
            }
            else
            {
                Require(credit.Status == Result::Deferred, "Shadow Latest producer reservation was rejected");
                ++stream.Stats.LatestSkipped;
            }
        }
        else ++stream.Stats.LatestSkipped;
    }
    return final;
}

void CrossAdapterShadows::RetireFrame(CompletionPoint tail)
{
    for (auto& stream : _streams)
    {
        if (!stream || !stream->AwaitingTail) continue;
        const auto finalize = stream->AwaitingTail->Tail;
        Require(tail.IsValid() && tail.Timeline() == finalize.Timeline() && tail.Value() > finalize.Value(),
            "Shadow frame retirement needs the actual primary DIRECT tail");
        stream->AwaitingTail->Tail = tail;
        stream->AwaitingTail = nullptr;
    }
}

void CrossAdapterShadows::RetireAtlas(GDX12ShadowAtlas atlas)
{
    auto& stream = _streams[atlas == GDX12ShadowAtlas::Sun ? 0 : 1];
    if (!stream) return;
    Require(!stream->AwaitingTail, "Shadow atlas retirement requires the primary frame tail");
    if (stream->Bank)
    {
        // Policy/ownership changes may arrive while Latest copies still read the old epoch.
        for (auto adapter : { _primary.GetSubmissionAdapterId(), _secondary.GetSubmissionAdapterId() })
            for (auto queue : { QueueKind::Direct, QueueKind::Copy })
                _runtime.GetQueue({ adapter, queue })->Flush();
        _runtime.RetireBank(*stream->Bank);
    }
    stream.reset();
}

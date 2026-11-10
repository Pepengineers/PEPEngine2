#include "Rendering/Grass/BistroGrassShadowReplica.h"

#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <limits>
#include <stdexcept>

namespace
{
    using namespace GDX12Submission;
    using Graph = GDX12RenderGraph;
    using GDX12Snapshot::PhysicalView;
    using GDX12Snapshot::Result;
    using GDX12Snapshot::Ticket;

    void Require(bool condition, const char* message)
    {
        if (!condition) throw std::logic_error(message);
    }

    bool OnDevice(ID3D12DeviceChild* resource, GDX12Device& device)
    {
        ComPtr<ID3D12Device14> owner;
        return resource && SUCCEEDED(resource->GetDevice(IID_PPV_ARGS(&owner))) &&
            owner.Get() == device.GetDevice().Get();
    }

    bool ValidView(const PhysicalView& view, GDX12Device& device)
    {
        return view.Resource && OnDevice(view.Resource->D3DResource.Get(), device) &&
            view.Identity.Adapter == device.GetSubmissionAdapterId() && view.Identity.Id != ResourceId::Invalid &&
            view.Identity.ResourceEpoch != Epoch::Invalid && view.Identity.Allocation != AllocationId::Invalid;
    }

    bool MatchesVersion(const PhysicalView& view, TransferVersion version)
    {
        return version.IsValid() ? version.Transfer == view.Identity.Transfer &&
            version.VersionEpoch == view.Identity.ResourceEpoch : view.Identity.Transfer == TransferId::Invalid;
    }

    using Patch = GDX12GrassRenderer::ShadowPatch;
    using Group = BistroGrassShadowReplica::GroupRequest;
    using PatchIndex = std::unordered_map<UINT, const Patch*>;

    PatchIndex IndexPatches(const BistroGrassShadowReplica::FrozenScene& capture)
    {
        PatchIndex patches;
        for (const auto& patch : capture.Scene.Patches)
            Require(patches.emplace(patch.Handle.Index, &patch).second, "Duplicate live grass primitive");
        return patches;
    }

    bool MatchGroup(const Group& group, const PatchIndex& patches)
    {
        if (group.Patches.empty()) return true;
        if (group.Output.StaticOnly || group.ReplicaGroup == UINT_MAX ||
            !std::isfinite(group.Eye.x) || !std::isfinite(group.Eye.y) || !std::isfinite(group.Eye.z))
            return false;
        std::unordered_set<UINT> sources;
        return std::all_of(group.Patches.begin(), group.Patches.end(), [&](const auto& handle)
        {
            const auto patch = patches.find(handle.Index);
            return patch != patches.end() && patch->second->Handle == handle && sources.insert(handle.Index).second;
        });
    }

    struct NativeSlot
    {
        NativeSlot(GDX12Device& device, std::shared_ptr<GDX12DescriptorHeap> heap,
            GDX12GrassRenderer::ShadowMaterials materials) : Heap(std::move(heap))
        {
            Resources.Device = &device;
            Resources.FrameConstants.resize(1);
            Renderer = std::make_unique<GDX12GrassRenderer>(&Resources, materials);
        }

        std::shared_ptr<GDX12DescriptorHeap> Heap;
        GDX12DeviceResources Resources;
        std::unique_ptr<GDX12GrassRenderer> Renderer;
    };

    class FieldPass final : public GDX12RenderPass
    {
    public:
        using Record = std::function<void(GDX12CommandList&, const Graph::BoundResources&)>;
        FieldPass(const char* name, Placement lane, Graph::Access source, Graph::Access dest, Record record)
            : _name(name), _lane(lane), _source(source), _dest(dest), _record(std::move(record)) {}
        bool SupportsGraphQueue(QueueKind queue) const override { return queue == _lane.Queue; }
        void RecordRenderGraph(GDX12RenderGraphBuilder& graph) override
        {
            graph.SetName(_name);
            graph.SetPlacement(_lane);
            graph.DeclareCompleteAccesses();
            graph.HasSideEffects();
            for (const auto& access : {_source, _dest})
            {
                if (access.Version.IsValid()) graph.UseVersion(access.Id, access.Usage, access.Version, access.State);
                else graph.Use(access.Id, access.Usage, access.State);
            }
        }
        void RecordPlacedCommands(GDX12CommandList* list, const Graph::BoundResources& resources) override
        { _record(*list, resources); }
    private:
        const char* _name;
        Placement _lane;
        Graph::Access _source, _dest;
        Record _record;
    };
}

struct BistroGrassShadowReplica::Impl
{
    struct Staging
    {
        PhysicalView View;
        Ticket User;
    };

    Impl(RenderMGPURuntime& runtime, GDX12Device& primary, GDX12Device& secondary)
        : Runtime(runtime), Primary(primary), DirectA{primary.GetSubmissionAdapterId(), QueueKind::Direct},
          CopyA{DirectA.Adapter, QueueKind::Copy}, DirectB{secondary.GetSubmissionAdapterId(), QueueKind::Direct},
          CopyB{DirectB.Adapter, QueueKind::Copy}, Stream(runtime.NewTransferId())
    {}

    RenderMGPURuntime& Runtime;
    GDX12Device& Primary;
    const Placement DirectA, CopyA, DirectB, CopyB;
    const TransferId Stream;
    Epoch LayoutEpoch = Epoch::Invalid;
    D3D12_RESOURCE_DESC Desc = {};
    bool Configured = false;
    std::uint64_t Invocation = 0;
    std::shared_ptr<GDX12Snapshot::Transport> Bank;
    std::array<Staging, GDX12Snapshot::SlotCount> Sources;
    std::shared_ptr<const ImportedField> Current;
    Ticket Publication;
    std::array<std::uint64_t, 2> OpenReaders = {};
    bool ReaderSubmitted = false;
    // Three frames of primary/secondary sun/local. Runtime owners keep a slot
    // busy until the last issued plan using it retires.
    std::array<std::shared_ptr<NativeSlot>, 12> RasterSlots;

    std::shared_ptr<NativeSlot> ReserveRaster(GDX12Device& device,
        const std::shared_ptr<GDX12DescriptorHeap>& heap, GDX12GrassRenderer::ShadowMaterials materials)
    {
        auto free = RasterSlots.end();
        for (auto it = RasterSlots.begin(); it != RasterSlots.end(); ++it)
        {
            if (it->use_count() > 1) continue;
            if (*it && (*it)->Resources.Device == &device && (*it)->Heap == heap)
            {
                (*it)->Renderer->SetShadowMaterials(materials);
                return *it;
            }
            if (free == RasterSlots.end()) free = it;
        }
        if (free == RasterSlots.end()) return {};
        *free = std::make_shared<NativeSlot>(device, heap, materials);
        return *free;
    }

    bool ValidField(GDX12Device& device, const FrozenScene& capture, const LocalField& field)
    {
        const auto adapter = device.GetSubmissionAdapterId();
        if (Runtime.IsStopped() || (adapter != DirectA.Adapter && adapter != DirectB.Adapter)) return false;
        const auto* queue = Runtime.GetQueue({ adapter, QueueKind::Direct });
        if (!queue || !capture.Frame.IsValid() || !(capture.Frame == field.Frame) || !field.Owner ||
            !field.Ready.IsValid() || field.Ready.Timeline().Producer.Location.Adapter != adapter ||
            !ValidView(field.View, device) || !MatchesVersion(field.View, field.Version) ||
            (!field.Version.IsValid() && (!(field.Ready.Timeline() == queue->GetTimelineIdentity()) ||
                field.Ready.Value() > queue->GetLastCompletionPoint().Value())))
            return false;
        const auto desc = field.View.Resource->D3DResource->GetDesc();
        return field.Manifest.ElementCount && desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
            desc.Width == UINT64(field.Manifest.ElementCount) * sizeof(XMFLOAT4);
    }

    bool Configure(UINT64 bytes)
    {
        if (!Bank)
        {
            LayoutEpoch = Runtime.NewEpoch();
            Desc = CD3DX12_RESOURCE_DESC::Buffer(bytes);
            Bank = Runtime.CreateBank(Stream, LayoutEpoch, DirectA.Adapter, DirectB.Adapter, Desc);
            Configured = true;
        }
        if (Configured && Desc.Width != bytes)
        {
            const auto next = Runtime.NewEpoch();
            if (!Bank->Reset(next)) return false;
            LayoutEpoch = next;
            Sources = {};
            Configured = false;
        }
        if (!Configured)
        {
            Desc = CD3DX12_RESOURCE_DESC::Buffer(bytes);
            if (!Runtime.TryReconfigureBank(*Bank, Desc)) return false;
            Configured = true;
        }
        return true;
    }

    Staging* FreeSource()
    {
        for (auto& source : Sources)
        {
            if (source.User.Id() && !Bank->SourceReusable(source.User)) continue;
            if (!source.View.Resource)
            {
                auto resource = std::make_shared<GDX12Resource>();
                resource->D3DResource = Primary.CreateResource(D3D12_HEAP_TYPE_DEFAULT, Desc, D3D12_RESOURCE_STATE_COMMON);
                source.View = {std::move(resource),
                    {Runtime.NewResourceId(), DirectA.Adapter, LayoutEpoch, Stream, Runtime.NewAllocationId()}};
            }
            return &source;
        }
        return nullptr;
    }
};

BistroGrassShadowReplica::BistroGrassShadowReplica(RenderMGPURuntime& runtime, GDX12Device& primary, GDX12Device& secondary)
    : _impl(std::make_unique<Impl>(runtime, primary, secondary))
{}
BistroGrassShadowReplica::~BistroGrassShadowReplica() = default;

void BistroGrassShadowReplica::CollectRetired()
{
    for (const auto& slot : _impl->RasterSlots)
        if (slot && slot.use_count() == 1) slot->Renderer->ReleaseShadowFrame();
}

std::shared_ptr<const BistroGrassShadowReplica::ImportedField> BistroGrassShadowReplica::SubmitField(
    const Field& field, CompletionPoint prefix)
{
    auto& s = *_impl;
    if (s.Runtime.IsStopped()) return {};
    Require(!s.Current, "Finish the previous grass shadow field before submitting another");
    auto* direct = s.Runtime.GetQueue(s.DirectA);
    const auto issuedPrefix = direct ? direct->GetLastCompletionPoint() : CompletionPoint{};
    if (!direct || !s.Runtime.GetQueue(s.CopyA) || !s.Runtime.GetQueue(s.CopyB) || !s.Runtime.GetQueue(s.DirectB) ||
        !prefix.IsValid() || !(prefix.Timeline() == issuedPrefix.Timeline()) || prefix.Value() != issuedPrefix.Value() ||
        !field.Frame.IsValid() || !field.Owner || !field.Manifest.ElementCount ||
        !ValidView(field.Primary, s.Primary) || !MatchesVersion(field.Primary, field.SourceVersion) ||
        field.Primary.Resource->GetCurrentState() != D3D12_RESOURCE_STATE_COMMON)
        return {};
    const auto desc = field.Primary.Resource->D3DResource->GetDesc();
    const UINT64 bytes = UINT64(field.Manifest.ElementCount) * sizeof(XMFLOAT4);
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || desc.Width != bytes) return {};
    CollectRetired();
    if (!s.Configure(bytes)) return {};
    auto* staging = s.FreeSource();
    if (!staging) return {};

    auto result = std::make_shared<ImportedField>();
    result->Frame = field.Frame;
    result->Manifest = field.Manifest;
    Require(s.Invocation != (std::numeric_limits<std::uint64_t>::max)(), "Grass shadow invocation exhausted");
    const TransferVersion version{s.Stream, s.LayoutEpoch, static_cast<ProducerInvocationId>(++s.Invocation)};
    auto& bank = *s.Bank;
    const auto credit = bank.Reserve(version, SnapshotReadMode::ExactVersion, *direct,
        staging->View.Resource->D3DResource, staging->View.Identity.Allocation);
    if (credit.Status == Result::Deferred) bank.CancelDeferredExact(version);
    if (credit.Status != Result::Accepted) return {};
    const auto ticket = credit.Publication;

    bool issued = false;
    try
    {
        if (!bank.PrepareExact(ticket))
        {
            Require(bank.Cancel(ticket), "Grass shadow copy reservation could not cancel");
            return {};
        }

        const auto& shared = bank.GetSlots().at(credit.SharedSlot);
        const auto& local = bank.GetSlots().at(credit.LocalSlot).Local;
        Graph graph;
        Graph::ExecutionBindings bindings;
        bindings.KeepAlive = {field.Owner, s.Bank};
        const auto bind = [&](const char* name, const PhysicalView& view, bool external, bool crossAdapter)
        {
            const auto id = external ? graph.Import(name) : graph.Create(name);
            graph.RegisterResource(id, view.Identity, crossAdapter);
            bindings.Views.push_back({id, view.Identity, view.Resource});
            return id;
        };
        const auto captured = bind("Current primary grass field", field.Primary, true, false);
        if (field.SourceVersion.IsValid())
        {
            graph.ImportVersion(captured, field.SourceVersion);
            bindings.ExactInputs.push_back({ captured, field.SourceVersion, prefix, field.Owner });
        }
        const auto source = bind("Grass shadow field staging", staging->View, false, false);
        const auto sharedOut = bind("Grass shadow field export", shared.Export, false, true);
        const auto sharedIn = bind("Grass shadow field import", shared.Import, false, true);
        const auto secondary = bind("Secondary grass shadow field", local, false, false);
        FieldPass stage("Stage primary grass shadow field", s.DirectA,
            {captured, Graph::Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE, field.SourceVersion},
            {source, Graph::Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST, version},
            [=](GDX12CommandList& commands, const Graph::BoundResources& views)
            { commands.GetCommandList()->CopyBufferRegion(views.Get(source), 0, views.Get(captured), 0, bytes); });
        FieldPass exportCopy("Export grass shadow field", s.CopyA,
            {source, Graph::Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE, version},
            {sharedOut, Graph::Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST, version},
            [&bank, ticket](GDX12CommandList& commands, const Graph::BoundResources&)
            { bank.RecordExportCopies(ticket, *commands.GetCommandList().Get()); });
        FieldPass importCopy("Import grass shadow field", s.CopyB,
            {sharedIn, Graph::Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE, version},
            {secondary, Graph::Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST, version},
            [&bank, ticket](GDX12CommandList& commands, const Graph::BoundResources&)
            { bank.RecordImportCopies(ticket, *commands.GetCommandList().Get()); });
        const auto stageId = graph.AddPass(stage);
        const auto exportId = graph.AddPass(exportCopy);
        const auto importId = graph.AddPass(importCopy);
        graph.DependsOn(exportId, stageId);
        graph.DependsOn(importId, exportId);
        graph.CompilePlan();

        const auto receipt = s.Runtime.TrySubmit(graph, std::move(bindings), [&](const auto& done)
        {
            const auto& points = done.Batches;
            Require(points.size() == 3, "Unexpected grass shadow transfer receipt");
            bank.CommitExact(ticket, points[0], points[1], points[1], points[2], points[2]);
            result->PrimaryStaged = points[0];
            result->ImportReady = points[2];
            result->Secondary = local;
            result->Version = version;
        });
        issued = receipt.has_value();
        if (!receipt)
        {
            Require(bank.Cancel(ticket), "Grass shadow transfer capacity retry could not cancel");
            return {};
        }
        staging->User = ticket;
        s.Publication = ticket;
        s.Current = result;
        return result;
    }
    catch (...)
    {
        if (!issued && !s.Runtime.IsStopped()) bank.Cancel(ticket);
        throw;
    }
}

std::optional<BistroGrassShadowReplica::Reader> BistroGrassShadowReplica::AcquireReader(
    const std::shared_ptr<const ImportedField>& field)
{
    auto& s = *_impl;
    if (!field || field != s.Current) return {};
    const auto open = std::find(s.OpenReaders.begin(), s.OpenReaders.end(), 0);
    if (open == s.OpenReaders.end()) return {};
    auto* queue = s.Runtime.GetQueue(s.DirectB);
    if (!queue) return {};
    auto lease = s.Bank->AcquireReader(s.Publication, *queue);
    if (!lease) return {};
    *open = lease->Lease.Id;
    return Reader{field, *open};
}

void BistroGrassShadowReplica::BindReader(const Reader& reader, CompletionPoint secondaryTail)
{
    auto& s = *_impl;
    const auto open = std::find(s.OpenReaders.begin(), s.OpenReaders.end(), reader.Id);
    Require(reader.Field && reader.Field == s.Current && reader.Id && open != s.OpenReaders.end(),
        "Grass shadow reader does not belong to the current field");
    Require(s.Bank->BindReader(reader.Id, secondaryTail), "Grass shadow reader receipt rejected");
    *open = 0;
    s.ReaderSubmitted = true;
}

void BistroGrassShadowReplica::CancelReader(const Reader& reader)
{
    auto& s = *_impl;
    const auto open = std::find(s.OpenReaders.begin(), s.OpenReaders.end(), reader.Id);
    Require(reader.Field && reader.Field == s.Current && reader.Id && open != s.OpenReaders.end(),
        "Grass shadow reader does not belong to the current field");
    Require(s.Bank->CancelReader(reader.Id), "Unsubmitted grass shadow reader could not cancel");
    *open = 0;
}

void BistroGrassShadowReplica::FinishField()
{
    auto& s = *_impl;
    if (!s.Current) return;
    Require(std::none_of(s.OpenReaders.begin(), s.OpenReaders.end(), [](auto reader) { return reader != 0; }),
        "Bind or cancel grass shadow readers before closing the field");
    Require(s.ReaderSubmitted ? s.Bank->ReleaseExact(s.Publication) : s.Bank->Cancel(s.Publication),
        "Grass shadow field demand could not close");
    s.Current.reset();
    s.Publication = {};
    s.ReaderSubmitted = false;
}

std::vector<BistroGrassShadowReplica::GroupBatches> BistroGrassShadowReplica::MakeBatches(
    GDX12Device& device, const FrozenScene& capture, const LocalField& field, const std::vector<GroupRequest>& groups)
{
    std::vector<GroupBatches> result(groups.size());
    const auto validField = _impl->ValidField(device, capture, field);
    const auto patches = IndexPatches(capture);
    for (size_t index = 0; index < groups.size(); ++index)
    {
        const auto& group = groups[index];
        if (group.Patches.empty()) { result[index].emplace(); continue; }
        if (!validField || !MatchGroup(group, patches)) continue;

        auto& batches = result[index].emplace();
        for (const auto& handle : group.Patches)
            batches.push_back({ capture.Frame, group.ReplicaGroup, handle.Index, &device, field.Owner,
                false, field.Ready, {{ { field.View.Resource, field.View.Identity, field.Version, field.Owner },
                    field.View.Resource->D3DResource, Graph::Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE }} });
    }
    return result;
}

BistroGrassShadowReplica::PreparedAtlas BistroGrassShadowReplica::Prepare(GDX12Device& device,
    std::shared_ptr<GDX12DescriptorHeap> heap, const FrozenScene& capture, const LocalField& field,
    const std::vector<GroupRequest>& groups, const MaterialBinding& materials)
{
    using Mode = Graph::Mode;
    PreparedAtlas result;
    result.Groups.resize(groups.size());
    bool needsGrass = false;
    for (size_t index = 0; index < groups.size(); ++index)
        if (groups[index].Patches.empty()) result.Groups[index].emplace();
        else needsGrass = true;
    if (!needsGrass || !_impl->ValidField(device, capture, field) ||
        !heap || !OnDevice(heap->GetHeap().Get(), device) ||
        heap->GetType() != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV ||
        heap->GetFlags() != D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE ||
        heap->GetNumDescriptors() < Texture2D_StartIndex + Texture2D_RangeLength)
        return result;

    const PhysicalView materialTable{ materials.Table.View.Native, materials.Table.View.Identity };
    if (!materials.Owner || !materials.Table.SRV.ptr || !ValidView(materialTable, device) ||
        materialTable.Identity.Transfer != TransferId::Invalid || materials.Table.View.Version.IsValid())
        return result;
    const auto desc = materialTable.Resource->D3DResource->GetDesc();
    const auto count = desc.Width / sizeof(GDX12MaterialConstants);
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || count == 0 || count > UINT_MAX ||
        desc.Width % sizeof(GDX12MaterialConstants))
        return result;
    for (const auto& mask : materials.AlphaReads)
        if (!ValidView(mask.View, device) || mask.View.Identity.Transfer != TransferId::Invalid || mask.Version.IsValid() ||
            mask.View.Resource->D3DResource->GetDesc().Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            mask.Mode != Mode::Read || !(mask.State & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE))
            return result;

    std::shared_ptr<GDX12GrassFieldSnapshot> snapshot;
    try
    {
        snapshot = std::make_shared<GDX12GrassFieldSnapshot>(*heap, *field.View.Resource, field.Manifest, field.View.Resource);
    }
    catch (const std::invalid_argument&)
    {
        return result;
    }
    const auto validWind = [&](const Patch& patch)
    {
        const auto id = patch.Instance.WindId;
        if (!id || !capture.Scene.Controls.WindEnabled || capture.Scene.Controls.WindQuality <= 0) return true;
        const auto wind = std::find_if(capture.Scene.Wind.begin(), capture.Scene.Wind.end(),
            [&](const auto& input) { return input.Id == id; });
        return wind == capture.Scene.Wind.end() || snapshot->MatchesWind(*wind, capture.Scene.Controls.WindQuality);
    };

    const auto patches = IndexPatches(capture);
    const auto mapped = [&](const Patch& patch)
    {
        const UINT primary = patch.Instance.MaterialIndex;
        return primary < materials.ToLocal.size() ? materials.ToLocal[primary] : UINT_MAX;
    };
    std::unordered_map<UINT, UINT> nativePatches;
    std::vector<GDX12GrassInstance> instances;
    std::vector<size_t> admitted;
    for (size_t index = 0; index < groups.size(); ++index)
    {
        const auto& group = groups[index];
        if (group.Patches.empty() || !MatchGroup(group, patches)) continue;
        const bool valid = std::all_of(group.Patches.begin(), group.Patches.end(), [&](const auto& handle)
        {
            const auto& patch = *patches.at(handle.Index);
            return mapped(patch) < count && validWind(patch);
        });
        if (!valid) continue;
        admitted.push_back(index);
        for (const auto& handle : group.Patches)
        {
            if (!nativePatches.try_emplace(handle.Index, static_cast<UINT>(instances.size())).second) continue;
            const auto& patch = *patches.at(handle.Index);
            auto instance = patch.Instance;
            instance.MaterialIndex = mapped(patch);
            instances.push_back(std::move(instance));
        }
    }
    if (instances.empty()) return result;

    const auto slot = _impl->ReserveRaster(device, heap, { heap.get(), materialTable.Resource.get(),
        materials.Table.SRV, heap->GetGPUHandle(Texture2D_StartIndex), static_cast<UINT>(count) });
    if (!slot) return result;

    const GDX12GrassRenderer::DisplayedFields snapshots{ snapshot, snapshot, true };
    RenderPipelineCommonData common;
    slot->Renderer->BeginFrame(common, std::move(instances), capture.Scene.Wind, capture.Scene.Controls, {}, 0, &snapshots);
    if (slot->Renderer->PatchCount() != nativePatches.size()) return result;
    result.Preparation.push_back({ "Bistro grass shadow roots", {},
        [slot](GDX12CommandList& commands) { slot->Renderer->PrepareGPU(&commands); } });

    for (const auto index : admitted)
    {
        auto& prepared = result.Groups[index].emplace();
        const auto& group = groups[index];
        CrossAdapterShadowRaster::ApplicationView application;
        application.Stamp = capture.Frame;
        application.Accesses = {
            { field.View, Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, field.Version },
            { materialTable, Mode::Read, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE } };
        application.Accesses.insert(application.Accesses.end(), materials.AlphaReads.begin(), materials.AlphaReads.end());
        application.Owners = { slot, field.Owner, materials.Owner };
        std::vector<UINT> drawPatches;
        for (const auto& handle : group.Patches)
        {
            application.Sources.push_back(handle.Index);
            drawPatches.push_back(nativePatches.at(handle.Index));
        }
        for (UINT face = 0; face < group.Output.ViewCount(); ++face)
        {
            GDX12GeometryContext view;
            view.Stage = GDX12GeometryStage::Shadow;
            view.ViewProjection = Matrix(group.Output.Views[face].ViewProj).Transpose();
            view.Eye = group.Eye;
            view.Filter = GDX12GeometryFilter::Dynamic;
            view.Key = index * 6 + face;
            view.DepthFormat = DXGI_FORMAT_D32_FLOAT;
            view.BatchCount = 1;
            for (const auto patch : drawPatches) slot->Renderer->PrepareView(view, patch);

            auto& native = prepared[face].emplace(application);
            const auto recorded = std::make_shared<std::vector<std::uint64_t>>();
            native.RecordedSources = recorded;
            native.Draw = [slot, view, drawPatches, sources = native.Sources, recorded](GDX12CommandList& commands)
            {
                recorded->clear();
                auto range = view;
                std::vector<GDX12RenderBatch> batches;
                batches.reserve(1);
                for (size_t i = 0; i < drawPatches.size(); ++i)
                {
                    const UINT patch = drawPatches[i];
                    range.BatchOffset = patch;
                    if (!slot->Renderer->Expand(&commands, range, patch, true)) continue;
                    slot->Renderer->GatherBatches(range, batches);
                    Require(batches.size() == 1, "Expanded grass shadow patch must emit one native batch");
                    GDX12EmitRenderBatch(&commands, range, batches.front());
                    recorded->push_back(sources[i]);
                    batches.clear();
                }
            };
        }
    }
    return result;
}

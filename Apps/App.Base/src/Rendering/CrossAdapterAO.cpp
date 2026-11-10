#include "App.Base/Rendering/CrossAdapterAO.h"
#include "Common/ConsoleVariables.h"
#include <algorithm>
#include <functional>
#include <limits>
#include <stdexcept>
#include <utility>

namespace
{
using AO = GDX12AmbientOcclusionPass;
using Graph = GDX12RenderGraph;
using namespace GDX12Submission;
using namespace GDX12Snapshot;

enum Channel : size_t { Depth, Normals, Visibility, ChannelCount };
enum Stage : size_t { Pack, ExportInputs, ImportInputs, Evaluate, ExportVisibility, ImportVisibility, Finalize, StageCount };

AutoConsoleVariable<bool> MultiGPU(L"Render.AO.MultiGPU", false,
    L"Evaluate the current frame's ambient occlusion on the secondary adapter when available",
    EConsoleVariableFlags::Default);

void Require(bool condition, const char* message)
{
    if (!condition) throw std::logic_error(message);
}

class Batch final : public GDX12RenderPass
{
public:
    using Recorder = std::function<void(GDX12CommandList&, const Graph::BoundResources&)>;

    Batch(const char* name, Placement placement, std::vector<Graph::Access> accesses, Recorder recorder)
        : _name(name), _placement(placement), _accesses(std::move(accesses)), _record(std::move(recorder)) {}

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
    { _record(*list, resources); }

private:
    const char* _name;
    Placement _placement;
    std::vector<Graph::Access> _accesses;
    Recorder _record;
};

AO::PlacedTexture BorrowTexture(const Graph::LegacyBoundaryView& boundary)
{
    auto& texture = *boundary.Texture;
    return { boundary.Native.get(),
        texture.HasSRV() ? texture.GetSRV()->GPUHandle : D3D12_GPU_DESCRIPTOR_HANDLE{},
        texture.HasUAV() ? texture.GetUAV()->GPUHandle : D3D12_GPU_DESCRIPTOR_HANDLE{} };
}

struct Transfer
{
    TransferVersion Version;
    Reservation Credit;
    std::optional<PreparedRead> Reader;
    Graph::Resource Source, Export, Import, Local;
};
}

struct CrossAdapterAO::Frame
{
    std::array<PhysicalView, ChannelCount> Sources;
    std::shared_ptr<AO::PlacedSlot> Pack, Evaluate, Finalize;
    std::vector<std::unique_ptr<GDX12Descriptor>> Descriptors;
    std::array<std::shared_ptr<GDX12Resource>, 3> BoundaryOwners;
    CompletionPoint Tail;

    AO::PlacedTexture Describe(const PhysicalView& view, GDX12DescriptorHeap& heap, bool srv, bool uav)
    {
        AO::PlacedTexture texture{view.Resource.get()};
        auto* native = view.Resource->D3DResource.Get();
        const auto desc = native->GetDesc();
        if (srv)
        {
            auto descriptor = std::make_unique<GDX12Descriptor>();
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Format = desc.Format;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Texture2D.MipLevels = 1;
            descriptor->InitAsSRV(native, &srvDesc, &heap,
                heap.GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength));
            texture.SRV = descriptor->GPUHandle;
            Descriptors.push_back(std::move(descriptor));
        }
        if (uav)
        {
            auto descriptor = std::make_unique<GDX12Descriptor>();
            D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
            uavDesc.Format = desc.Format;
            uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            descriptor->InitAsUAV(native, nullptr, &uavDesc, &heap,
                heap.GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength));
            texture.UAV = descriptor->GPUHandle;
            Descriptors.push_back(std::move(descriptor));
        }
        return texture;
    }
};

bool IsMultiGPUAmbientOcclusionEnabled() { return MultiGPU.GetValue(); }

CrossAdapterAO::CrossAdapterAO(AO& local, GDX12DeviceResources& primary, GDX12Device& secondary,
    RenderMGPURuntime& runtime) : _local(local), _primary(primary), _runtime(runtime)
{
    _secondary.Device = &secondary;
    for (auto& stream : _streams) stream = runtime.NewTransferId();
}

CrossAdapterAO::~CrossAdapterAO() = default;

void CrossAdapterAO::Execute(GDX12CommandList* commands)
{
    auto input = std::exchange(_frozen, std::nullopt);
    if (input) _local.RecordLocalFrame(commands, *input);
    else _local.Execute(commands);
}

bool CrossAdapterAO::PrepareLayout(const AO::FrameInput& input)
{
    if (!_secondaryReady)
    {
        _secondary.SRV_UAV_Heap = std::make_unique<GDX12DescriptorHeap>(_secondary.Device,
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, TextureResources_StartIndex + TextureResources_RangeLength,
            D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
        _evaluate.InitializePlacedDevice(_secondary);
        _secondaryReady = true;
    }

    if (_width != input.Width || _height != input.Height || _resolution != input.Settings.Resolution)
    {
        const auto progress = _primary.Device->GetCommandQueue()->GetCompletionProgress();
        for (const auto& frame : _frames)
            if (frame && (frame.use_count() != 1 || (frame->Tail.IsValid() && !frame->Tail.IsReachedBy(progress))))
                return false;
        _frames = {};
        const auto epoch = _runtime.NewEpoch();
        for (const auto& bank : _banks)
            if (bank) Require(bank->Reset(epoch), "AO bank still has an unbound reservation");
        _epoch = epoch;
        _width = input.Width;
        _height = input.Height;
        _resolution = input.Settings.Resolution;
        const bool half = _resolution == GDX12AOResolution::Half;
        const UINT w = half ? (_width + 1) / 2 : _width, h = half ? (_height + 1) / 2 : _height;
        _layout = { CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32_FLOAT, w, h, 1, 1),
            CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16G16B16A16_FLOAT, w, h, 1, 1),
            CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32_FLOAT, w, h, 1, 1) };
        _configured.fill(false);
    }
    for (size_t i = 0; i < ChannelCount; ++i)
    {
        if (_configured[i]) continue;
        if (!_banks[i])
        {
            const auto primary = _primary.Device->GetSubmissionAdapterId();
            const auto secondary = _secondary.Device->GetSubmissionAdapterId();
            const bool returning = i == Visibility;
            _banks[i] = _runtime.CreateBank(_streams[i], _epoch,
                returning ? secondary : primary, returning ? primary : secondary, _layout[i]);
        }
        else if (!_runtime.TryReconfigureBank(*_banks[i], _layout[i])) return false;
        _configured[i] = true;
    }
    return true;
}

PhysicalView CrossAdapterAO::BindBoundary(const Graph::LegacyBoundaryView& view)
{
    Require(view.Texture && view.Native && view.Native.get() == view.Texture->GetResource() &&
        view.Native->GetCurrentState() == D3D12_RESOURCE_STATE_COMMON, "Invalid legacy AO handoff");
    auto& entry = _boundaryIds[view.Native.get()];
    if (entry.Native.expired())
        entry = { view.Native, { _runtime.NewResourceId(), _primary.Device->GetSubmissionAdapterId(),
            _runtime.NewEpoch(), TransferId::Invalid, _runtime.NewAllocationId() } };
    return { view.Native, entry.Identity };
}

void CrossAdapterAO::RetireFrame(CompletionPoint tail)
{
    if (!_awaitingTail) return;
    auto& direct = *_primary.Device->GetCommandQueue();
    Require(tail.IsValid() && tail.Timeline() == direct.GetTimelineIdentity() &&
        tail.Value() > _awaitingTail->Tail.Value(), "AO frame retirement needs the submitted DIRECT tail");
    std::exchange(_awaitingTail, nullptr)->Tail = tail;
}

std::optional<CompletionPoint> CrossAdapterAO::Submit(const Graph::LegacyBoundary& boundary, CompletionPoint prefix)
{
    Require(boundary.Replaced == this && !_frozen && !_awaitingTail, "AO replacement called outside its frame");
    auto& direct = *_primary.Device->GetCommandQueue();
    Require(prefix.IsValid() && prefix.Timeline() == direct.GetTimelineIdentity() &&
        prefix.Value() == direct.GetLastCompletionPoint().Value(), "AO replacement needs the current DIRECT prefix");

    _frozen = _local.CaptureFrameInput();
    const auto& input = *_frozen;
    const UINT minimum = input.Settings.Resolution == GDX12AOResolution::Half ? 2 : 1;
    if (!input.Settings.IsActive() || (std::max)(input.Width, input.Height) <= minimum) return std::nullopt;

    std::array<Transfer, ChannelCount> transfers{};
    const auto rollback = [&]
    {
        for (size_t i = 0; i < transfers.size(); ++i)
        {
            auto& transfer = transfers[i];
            if (transfer.Credit.Status == Result::Accepted)
                Require(_banks[i]->Cancel(transfer.Credit.Publication), "Unsubmitted AO reservation failed to cancel");
            else if (transfer.Credit.Status == Result::Deferred)
                _banks[i]->CancelDeferredExact(transfer.Version);
            transfer.Credit = {};
        }
    };
    bool issued = false;
    try
    {
        if (!PrepareLayout(input)) return std::nullopt;
        const auto progress = direct.GetCompletionProgress();
        auto it = std::find_if(_frames.begin(), _frames.end(), [&](const auto& frame)
        {
            return !frame || (frame.use_count() == 1 && (!frame->Tail.IsValid() || frame->Tail.IsReachedBy(progress)));
        });
        if (it == _frames.end()) return std::nullopt;
        const Placement directA{_primary.Device->GetSubmissionAdapterId(), QueueKind::Direct};
        const Placement computeB{_secondary.Device->GetSubmissionAdapterId(), QueueKind::Compute};
        const Placement copyA{directA.Adapter, QueueKind::Copy}, copyB{computeB.Adapter, QueueKind::Copy};
        auto* compute = _runtime.GetQueue(computeB);
        Require(compute != nullptr, "Secondary AO COMPUTE queue unavailable");

        if (!*it)
        {
            auto frame = std::make_shared<Frame>();
            frame->Pack = _local.CreatePlacedSlot(AO::PlacedStage::Pack, _width, _height, _resolution);
            frame->Evaluate = _evaluate.CreatePlacedSlot(AO::PlacedStage::Evaluate, _width, _height, _resolution);
            frame->Finalize = _local.CreatePlacedSlot(AO::PlacedStage::Finalize, _width, _height, _resolution);
            for (size_t i = 0; i < frame->Sources.size(); ++i)
            {
                auto* device = i == Visibility ? _secondary.Device : _primary.Device;
                auto desc = _layout[i];
                desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
                auto resource = std::make_shared<GDX12Resource>();
                resource->D3DResource = device->CreateResource(D3D12_HEAP_TYPE_DEFAULT, desc, D3D12_RESOURCE_STATE_COMMON);
                frame->Sources[i] = { std::move(resource), { _runtime.NewResourceId(), device->GetSubmissionAdapterId(),
                    _epoch, _streams[i], _runtime.NewAllocationId() } };
            }
            *it = std::move(frame);
        }
        auto& frame = **it;
        frame.Descriptors.clear();
        frame.BoundaryOwners = {};
        frame.Tail = {};

        Require(_invocation < (std::numeric_limits<std::uint64_t>::max)(), "AO invocation ID exhausted");
        const ProducerInvocationId invocation{++_invocation};
        for (size_t i = 0; i < transfers.size(); ++i)
        {
            auto& transfer = transfers[i];
            auto& bank = *_banks[i];
            const auto& source = frame.Sources[i];
            const bool returning = i == Visibility;
            transfer.Version = { _streams[i], _epoch, invocation };
            transfer.Credit = bank.Reserve(transfer.Version, SnapshotReadMode::ExactVersion,
                returning ? *compute : direct, source.Resource->D3DResource, source.Identity.Allocation);
            if (transfer.Credit.Status != Result::Accepted || !bank.PrepareExact(transfer.Credit.Publication))
            {
                rollback();
                return std::nullopt;
            }
            transfer.Reader = bank.PrepareExactReader(transfer.Credit.Publication, returning ? direct : *compute);
            if (!transfer.Reader)
            {
                rollback();
                return std::nullopt;
            }
        }

        Require(boundary.Inputs.size() == 2 && boundary.Outputs.size() == 1, "Incomplete legacy AO boundary");
        const auto depth = BindBoundary(boundary.Inputs[0]), normals = BindBoundary(boundary.Inputs[1]);
        const auto output = BindBoundary(boundary.Outputs[0]);
        frame.BoundaryOwners = { depth.Resource, normals.Resource, output.Resource };
        for (auto identity = _boundaryIds.begin(); identity != _boundaryIds.end();)
            if (identity->second.Native.expired()) identity = _boundaryIds.erase(identity);
            else ++identity;

        auto& heapA = *_primary.SRV_UAV_Heap;
        auto& heapB = *_secondary.SRV_UAV_Heap;
        const bool half = _resolution == GDX12AOResolution::Half;
        AO::PlacedInputs pack{};
        pack.Depth = BorrowTexture(boundary.Inputs[0]);
        pack.Normals = BorrowTexture(boundary.Inputs[1]);
        pack.PreparedDepth = frame.Describe(frame.Sources[Depth], heapA, half, true);
        pack.PreparedNormals = frame.Describe(frame.Sources[Normals], heapA, half, true);
        AO::PlacedInputs evaluate{};
        evaluate.PreparedDepth = frame.Describe(transfers[Depth].Reader->View, heapB, true, false);
        evaluate.PreparedNormals = frame.Describe(transfers[Normals].Reader->View, heapB, true, false);
        evaluate.Visibility = frame.Describe(frame.Sources[Visibility], heapB, false, true);
        AO::PlacedInputs finalize = pack;
        finalize.Visibility = frame.Describe(transfers[Visibility].Reader->View, heapA, half, false);
        finalize.Output = BorrowTexture(boundary.Outputs[0]);
        _local.PreparePlacedSlot(frame.Pack, input, pack);
        _evaluate.PreparePlacedSlot(frame.Evaluate, input, evaluate);
        _local.PreparePlacedSlot(frame.Finalize, input, finalize);

        Graph graph;
        Graph::ExecutionBindings bindings;
        bindings.KeepAlive = { *it, _banks[Depth], _banks[Normals], _banks[Visibility] };
        const auto bind = [&](const char* name, const PhysicalView& view, bool external = false, bool shared = false)
        {
            const auto id = external ? graph.Import(name) : graph.Create(name);
            graph.RegisterResource(id, view.Identity, shared);
            bindings.Views.push_back({ id, view.Identity, view.Resource });
            return id;
        };
        bind("Frame depth", depth, true);
        bind("Frame normals", normals, true);
        bind("Frame visibility", output, true);
        constexpr const char* names[]{ "Prepared depth", "Prepared normals", "Evaluated visibility" };
        for (size_t i = 0; i < transfers.size(); ++i)
        {
            auto& transfer = transfers[i];
            const auto& views = _banks[i]->GetSlots().at(transfer.Credit.SharedSlot);
            transfer.Source = bind(names[i], frame.Sources[i]);
            transfer.Export = bind("AO shared export", views.Export, false, true);
            transfer.Import = bind("AO shared import", views.Import, false, true);
            transfer.Local = bind("AO local import", transfer.Reader->View);
        }

        const auto aoBatch = [&](const char* name, Placement placement, AO& recorder, const AO::PlacedSlot& slot)
        {
            std::vector<Graph::Access> accesses;
            std::vector<std::pair<Graph::Resource, ID3D12Resource*>> checks;
            for (const auto& access : recorder.GetPlacedAccesses(slot))
            {
                const auto view = std::find_if(bindings.Views.begin(), bindings.Views.end(),
                    [&](const auto& v) { return v.Native.get() == access.Resource; });
                Require(view != bindings.Views.end(), "Placed AO recorder declared an unbound view");
                TransferVersion version{};
                for (const auto& transfer : transfers)
                    if (view->View == transfer.Source || view->View == transfer.Local) version = transfer.Version;
                accesses.push_back({ view->View, access.Mode, access.State, version });
                checks.emplace_back(view->View, access.Native.Get());
            }
            return Batch(name, placement, std::move(accesses),
                [&recorder, &slot, checks = std::move(checks)](GDX12CommandList& list, const Graph::BoundResources& views)
                {
                    for (const auto& [view, native] : checks)
                        Require(views.Get(view) == native, "Placed AO descriptor does not match the bound view");
                    recorder.RecordPlacedSlot(&list, slot);
                });
        };
        const auto copyBatch = [&](const char* name, Placement placement, size_t first, size_t end, bool exporting)
        {
            std::vector<Graph::Access> accesses;
            for (size_t i = first; i < end; ++i)
            {
                const auto& transfer = transfers[i];
                accesses.push_back({ exporting ? transfer.Source : transfer.Import, Graph::Mode::Read,
                    D3D12_RESOURCE_STATE_COPY_SOURCE, transfer.Version });
                accesses.push_back({ exporting ? transfer.Export : transfer.Local, Graph::Mode::Write,
                    D3D12_RESOURCE_STATE_COPY_DEST, transfer.Version });
            }
            return Batch(name, placement, std::move(accesses),
                [&, first, end, exporting](GDX12CommandList& list, const Graph::BoundResources&)
                {
                    for (size_t i = first; i < end; ++i)
                    {
                        auto& native = *list.GetCommandList().Get();
                        const auto ticket = transfers[i].Credit.Publication;
                        if (exporting) _banks[i]->RecordExportCopies(ticket, native);
                        else _banks[i]->RecordImportCopies(ticket, native);
                    }
                });
        };

        auto packBatch = aoBatch("AO Pack", directA, _local, *frame.Pack);
        auto exportInputs = copyBatch("AO Export depth and normals", copyA, Depth, Visibility, true);
        auto importInputs = copyBatch("AO Import depth and normals", copyB, Depth, Visibility, false);
        auto evaluateBatch = aoBatch("AO Evaluate", computeB, _evaluate, *frame.Evaluate);
        auto exportVisibility = copyBatch("AO Export visibility", copyB, Visibility, ChannelCount, true);
        auto importVisibility = copyBatch("AO Import visibility", copyA, Visibility, ChannelCount, false);
        auto finalizeBatch = aoBatch("AO Finalize", directA, _local, *frame.Finalize);
        const std::array<Batch*, StageCount> batches{ &packBatch, &exportInputs, &importInputs, &evaluateBatch,
            &exportVisibility, &importVisibility, &finalizeBatch };
        for (auto* batch : batches) graph.AddPass(*batch);
        graph.CompilePlan();
        const auto& plan = graph.GetCompiledPlan();
        const std::array<Placement, StageCount> placements{ directA, copyA, copyB, computeB, copyB, copyA, directA };
        Require(plan.Batches.size() == placements.size(), "AO placed plan has an unexpected batch count");
        for (size_t i = 0; i < placements.size(); ++i)
            Require(plan.Batches[i].Location == placements[i], "AO placed plan has an unexpected batch order");

        const auto receipt = _runtime.TrySubmit(graph, std::move(bindings), [&](const Graph::ExecutionReceipt& submitted)
        {
            const auto& p = submitted.Batches;
            Require(p.size() == StageCount, "Incomplete AO receipt");
            for (size_t i = 0; i < transfers.size(); ++i)
            {
                const auto& transfer = transfers[i];
                auto& bank = *_banks[i];
                const bool returning = i == Visibility;
                const auto exported = p[returning ? ExportVisibility : ExportInputs];
                const auto imported = p[returning ? ImportVisibility : ImportInputs];
                bank.CommitExact(transfer.Credit.Publication, p[returning ? Evaluate : Pack],
                    exported, exported, imported, imported);
                Require(bank.BindReader(transfer.Reader->Id, p[returning ? Finalize : Evaluate]),
                    "AO reader receipt rejected");
                Require(bank.ReleaseExact(transfer.Credit.Publication), "AO Exact pin release failed");
            }
        });
        if (!receipt)
        {
            rollback();
            return std::nullopt;
        }
        issued = true;
        frame.Tail = receipt->Batches[Finalize];
        _lastFinalize = frame.Tail;
#ifdef _DEBUG
        _debugFrame.Input = input;
        for (size_t i = 0; i < transfers.size(); ++i)
        {
            _debugFrame.Source[i] = frame.Sources[i].Resource;
            _debugFrame.Imported[i] = transfers[i].Reader->View.Resource;
        }
#endif
        _awaitingTail = &frame;
        _local.UpdateHistory(input, true);
        _frozen.reset();
        return frame.Tail;
    }
    catch (...)
    {
        if (issued || _runtime.IsStopped()) throw;
        rollback();
        if (!_loggedFallback)
        {
            _loggedFallback = true;
            try { throw; }
            catch (const DxException& error) { OutputDebugStringW(error.ToString().c_str()); }
            catch (const std::exception& error) { OutputDebugStringA(error.what()); }
            catch (...) { OutputDebugStringA("Unknown MGPU AO pre-submit failure"); }
        }
        return std::nullopt;
    }
}

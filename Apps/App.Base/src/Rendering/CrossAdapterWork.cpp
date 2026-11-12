#include "App.Base/Rendering/CrossAdapterWork.h"
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include <algorithm>
#include <chrono>
#include <limits>
#include <stdexcept>
namespace
{
using Graph = GDX12RenderGraph;
using namespace GDX12Submission;
using namespace GDX12Snapshot;
using Clock = std::chrono::steady_clock;
void Require(bool value, const char* message) { if (!value) throw std::logic_error(message); }
class Batch final : public GDX12RenderPass
{
public:
    Batch(const char* name, Placement placement, std::vector<Graph::Access> uses,
        std::vector<std::pair<std::shared_ptr<GDX12Resource>, D3D12_RESOURCE_STATES>> resources,
        std::function<void(GDX12CommandList&)> record)
        : _name(name), _placement(placement), _uses(std::move(uses)), _resources(std::move(resources)),
          _record(std::move(record)) {}
    void RecordRenderGraph(GDX12RenderGraphBuilder& graph) override
    {
        graph.SetName(_name);
        graph.SetPlacement(_placement);
        graph.DeclareCompleteAccesses();
        graph.HasSideEffects();
        for (const auto& use : _uses)
            if (use.Version.IsValid()) graph.UseVersion(use.Id, use.Usage, use.Version, use.State);
            else graph.Use(use.Id, use.Usage, use.State);
    }
    bool SupportsGraphQueue(QueueKind queue) const override { return queue == _placement.Queue; }
    void RecordPlacedCommands(GDX12CommandList* commands, const Graph::BoundResources&) override
    {
        std::vector<D3D12_RESOURCE_STATES> previous;
        for (const auto& item : _resources) previous.push_back(item.first->GetCurrentState());
        for (const auto& item : _resources) item.first->SetCurrentState(item.second);
        try
        {
            _record(*commands);
            for (const auto& item : _resources)
                commands->ResourceBarrier({item.first->GetTransitionBarrier(item.second)});
        }
        catch (...)
        {
            for (size_t i = 0; i < _resources.size(); ++i) _resources[i].first->SetCurrentState(previous[i]);
            throw;
        }
        for (size_t i = 0; i < _resources.size(); ++i) _resources[i].first->SetCurrentState(previous[i]);
    }
private:
    const char* _name;
    Placement _placement;
    std::vector<Graph::Access> _uses;
    std::vector<std::pair<std::shared_ptr<GDX12Resource>, D3D12_RESOURCE_STATES>> _resources;
    std::function<void(GDX12CommandList&)> _record;
};
}

CrossAdapterWork::View CrossAdapterWork::Identify(std::shared_ptr<GDX12Resource> resource, GDX12Device& device)
{
    _identities.erase(std::remove_if(_identities.begin(), _identities.end(),
        [](const auto& entry) { return entry.Native.expired(); }), _identities.end());
    for (const auto& entry : _identities)
        if (entry.Native.lock() == resource) return {std::move(resource), entry.Id};
    ResourceIdentity id{_runtime.NewResourceId(), device.GetSubmissionAdapterId(), _runtime.NewEpoch(),
        TransferId::Invalid, _runtime.NewAllocationId()};
    _identities.push_back({resource, id});
    return {std::move(resource), id};
}

CrossAdapterWork::View CrossAdapterWork::Allocate(GDX12Device& device, UINT64 bytes)
{
    auto resource = std::make_shared<GDX12Resource>();
    resource->D3DResource = device.CreateResource(D3D12_HEAP_TYPE_DEFAULT,
        CD3DX12_RESOURCE_DESC::Buffer(bytes), D3D12_RESOURCE_STATE_COMMON);
    return Identify(std::move(resource), device);
}

void CrossAdapterWork::Wait(CompletionPoint point)
{
    auto* queue = _runtime.GetQueue(point.Timeline().Producer.Location);
    Require(queue && queue->GetTimelineIdentity() == point.Timeline(), "Unknown snapshot completion timeline");
    const auto start = Clock::now();
    queue->CPUWaitForFenceValue(point.Value());
    WaitMilliseconds += std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    _runtime.Poll();
}

CompletionPoint CrossAdapterWork::Run(const char* name, GDX12Device& device, std::vector<Use> uses, Record record)
{
    Graph graph;
    Graph::ExecutionBindings bindings;
    std::vector<Graph::Access> accesses;
    std::vector<std::pair<std::shared_ptr<GDX12Resource>, D3D12_RESOURCE_STATES>> resources;
    for (const auto& use : uses)
    {
        const auto id = graph.Import(name);
        graph.RegisterResource(id, use.Resource.Identity);
        bindings.Views.push_back({id, use.Resource.Identity, use.Resource.Resource});
        accesses.push_back({id, use.Mode, use.State, {}});
        resources.emplace_back(use.Resource.Resource, use.State);
    }
    Batch batch(name, {device.GetSubmissionAdapterId(), QueueKind::Direct}, std::move(accesses),
        std::move(resources), std::move(record));
    graph.AddPass(batch);
    graph.CompilePlan();
    const auto receipt = _runtime.TrySubmit(graph, std::move(bindings));
    Require(receipt && receipt->Batches.size() == 1, "Snapshot graph submission capacity exhausted");
    const auto done = receipt->Batches.back();
    Wait(done);
    return done;
}

CompletionPoint CrossAdapterWork::Transfer(Channel& channel, GDX12Device& producer, GDX12Device& consumer,
    UINT64 bytes, std::vector<Use> source, std::vector<Use> destination,
    std::function<void(GDX12CommandList&, GDX12Resource&)> pack,
    std::function<void(GDX12CommandList&, GDX12Resource&)> unpack)
{
    const auto a = producer.GetSubmissionAdapterId(), b = consumer.GetSubmissionAdapterId();
    if (!channel.Bank || channel.Bytes != bytes || channel.Producer != a || channel.Consumer != b)
    {
        if (channel.Bank) _runtime.RetireBank(*channel.Bank);
        channel.Id = _runtime.NewTransferId();
        channel.EpochId = _runtime.NewEpoch();
        channel.Bank = _runtime.CreateBank(channel.Id, channel.EpochId, a, b, CD3DX12_RESOURCE_DESC::Buffer(bytes));
        channel.Source = Allocate(producer, bytes);
        channel.Source.Identity.Transfer = channel.Id;
        channel.Source.Identity.ResourceEpoch = channel.EpochId;
        channel.Bytes = bytes;
        channel.Producer = a;
        channel.Consumer = b;
    }
    Require(channel.Invocation < (std::numeric_limits<UINT64>::max)(), "Snapshot invocation exhausted");
    const TransferVersion version{channel.Id, channel.EpochId, ProducerInvocationId{++channel.Invocation}};
    auto& bank = *channel.Bank;
    const auto credit = bank.Reserve(version, SnapshotReadMode::ExactVersion, *producer.GetCommandQueue(),
        channel.Source.Resource->D3DResource, channel.Source.Identity.Allocation);
    Require(credit.Status == Result::Accepted, "Snapshot transfer reservation was not accepted");
    bool submitted = false;
    try
    {
        Require(bank.PrepareExact(credit.Publication), "Snapshot exact transfer was not prepared");
        const auto reader = bank.PrepareExactReader(credit.Publication, *consumer.GetCommandQueue());
        Require(reader.has_value(), "Snapshot exact reader was not prepared");
        const auto& slots = bank.GetSlots().at(credit.SharedSlot);
        Graph graph;
        Graph::ExecutionBindings bindings;
        bindings.KeepAlive.push_back(channel.Bank);
        const auto bind = [&](const View& view, bool shared = false)
        {
            const auto id = graph.Import("Snapshot transfer resource");
            graph.RegisterResource(id, view.Identity, shared);
            bindings.Views.push_back({id, view.Identity, view.Resource});
            return id;
        };
        const auto from = bind(channel.Source), exported = bind(slots.Export, true);
        const auto imported = bind(slots.Import, true), local = bind(reader->View);
        std::vector<Graph::Access> packUses{{from, Graph::Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST, version}};
        std::vector<Graph::Access> unpackUses{{local, Graph::Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE, version}};
        std::vector<std::pair<std::shared_ptr<GDX12Resource>, D3D12_RESOURCE_STATES>> packResources{
            {channel.Source.Resource, D3D12_RESOURCE_STATE_COPY_DEST}};
        std::vector<std::pair<std::shared_ptr<GDX12Resource>, D3D12_RESOURCE_STATES>> unpackResources{
            {reader->View.Resource, D3D12_RESOURCE_STATE_COPY_SOURCE}};
        for (const auto& use : source)
        {
            packUses.push_back({bind(use.Resource), use.Mode, use.State, {}});
            packResources.emplace_back(use.Resource.Resource, use.State);
        }
        for (const auto& use : destination)
        {
            unpackUses.push_back({bind(use.Resource), use.Mode, use.State, {}});
            unpackResources.emplace_back(use.Resource.Resource, use.State);
        }
        Batch prepare("Snapshot pack", {a, QueueKind::Direct}, std::move(packUses), std::move(packResources),
            [&](GDX12CommandList& commands) { pack(commands, *channel.Source.Resource); });
        Batch send("Snapshot export", {a, QueueKind::Copy},
            {{from, Graph::Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE, version},
             {exported, Graph::Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST, version}}, {},
            [&](GDX12CommandList& commands) { bank.RecordExportCopies(credit.Publication, *commands.GetCommandList().Get()); });
        Batch receive("Snapshot import", {b, QueueKind::Copy},
            {{imported, Graph::Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE, version},
             {local, Graph::Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST, version}}, {},
            [&](GDX12CommandList& commands) { bank.RecordImportCopies(credit.Publication, *commands.GetCommandList().Get()); });
        Batch restore("Snapshot unpack", {b, QueueKind::Direct}, std::move(unpackUses), std::move(unpackResources),
            [&](GDX12CommandList& commands) { unpack(commands, *reader->View.Resource); });
        for (auto* batch : {&prepare, &send, &receive, &restore}) graph.AddPass(*batch);
        graph.CompilePlan();
        const auto receipt = _runtime.TrySubmit(graph, std::move(bindings), [&](const Graph::ExecutionReceipt& result)
        {
            const auto& points = result.Batches;
            Require(points.size() == 4, "Incomplete snapshot transfer receipt");
            bank.CommitExact(credit.Publication, points[0], points[1], points[1], points[2], points[2]);
            Require(bank.BindReader(reader->Id, points[3]) && bank.ReleaseExact(credit.Publication),
                "Snapshot transfer reader retirement failed");
        });
        Require(receipt.has_value(), "Snapshot graph submission capacity exhausted");
        submitted = true;
        Wait(receipt->Batches.back());
        return receipt->Batches.back();
    }
    catch (...)
    {
        if (!submitted && !_runtime.IsStopped()) bank.Cancel(credit.Publication);
        throw;
    }
}


UINT64 CrossAdapterWork::Copy(Channel& channel, GDX12Device& producer, GDX12Device& consumer,
    const std::vector<CopyRegion>& regions)
{
    if (regions.empty()) return 0;
    struct Copy { CopyRegion Region; UINT Subresource; D3D12_PLACED_SUBRESOURCE_FOOTPRINT Footprint; UINT64 Bytes; };
    std::vector<Copy> copies;
    std::vector<Use> reads, writes;
    UINT64 bytes = 0;
    const auto add = [](std::vector<Use>& uses, const View& view, GDX12RenderGraph::Mode mode, D3D12_RESOURCE_STATES state)
    {
        if (std::none_of(uses.begin(), uses.end(), [&](const auto& use) { return use.Resource.Resource == view.Resource; }))
            uses.push_back({view, mode, state});
    };
    for (const auto& region : regions)
    {
        Require(region.Source.Resource && region.Destination.Resource, "Missing snapshot copy resource");
        const auto desc = region.Source.Resource->D3DResource->GetDesc();
        const auto target = region.Destination.Resource->D3DResource->GetDesc();
        Require(desc.Dimension == target.Dimension && desc.Format == target.Format, "Incompatible snapshot copy resource");
        add(reads, region.Source, Graph::Mode::Read,
            region.Source.Resource->GetCurrentState() == D3D12_RESOURCE_STATE_GENERIC_READ ?
                D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_SOURCE);
        add(writes, region.Destination, Graph::Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST);
        if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
        {
            Require(desc.Width == target.Width, "Incompatible snapshot buffer size");
            bytes = (bytes + 511) & ~UINT64(511);
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT location{};
            location.Offset = bytes;
            copies.push_back({region, UINT_MAX, location, desc.Width});
            bytes += desc.Width;
            continue;
        }
        const UINT count = desc.MipLevels * (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : desc.DepthOrArraySize);
        Require(desc.Width == target.Width && desc.Height == target.Height && desc.DepthOrArraySize == target.DepthOrArraySize &&
            desc.MipLevels == target.MipLevels, "Incompatible snapshot texture size");
        const UINT first = region.SourceSubresource == UINT_MAX ? 0 : region.SourceSubresource;
        const UINT end = region.SourceSubresource == UINT_MAX ? count : first + 1;
        Require(first < count && end <= count, "Snapshot subresource outside source");
        for (UINT sub = first; sub < end; ++sub)
        {
            bytes = (bytes + 511) & ~UINT64(511);
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT location{};
            UINT64 size = 0;
            producer.GetDevice()->GetCopyableFootprints(&desc, sub, 1, 0, &location, nullptr, nullptr, &size);
            location.Offset = bytes;
            auto selected = region;
            selected.SourceSubresource = sub;
            selected.DestinationSubresource = region.DestinationSubresource == UINT_MAX ? sub : region.DestinationSubresource;
            Require(selected.DestinationSubresource < count, "Snapshot subresource outside destination");
            copies.push_back({selected, sub, location, size});
            bytes += size;
        }
    }
    Transfer(channel, producer, consumer, bytes, std::move(reads), std::move(writes),
        [&](GDX12CommandList& commands, GDX12Resource& payload)
        {
            for (const auto& copy : copies)
                if (copy.Subresource == UINT_MAX)
                    commands.GetCommandList()->CopyBufferRegion(payload.D3DResource.Get(), copy.Footprint.Offset,
                        copy.Region.Source.Resource->D3DResource.Get(), 0, copy.Bytes);
                else
                {
                    const CD3DX12_TEXTURE_COPY_LOCATION source(copy.Region.Source.Resource->D3DResource.Get(), copy.Region.SourceSubresource);
                    const CD3DX12_TEXTURE_COPY_LOCATION target(payload.D3DResource.Get(), copy.Footprint);
                    commands.GetCommandList()->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
                }
        },
        [&](GDX12CommandList& commands, GDX12Resource& payload)
        {
            for (const auto& copy : copies)
                if (copy.Subresource == UINT_MAX)
                    commands.GetCommandList()->CopyBufferRegion(copy.Region.Destination.Resource->D3DResource.Get(), 0,
                        payload.D3DResource.Get(), copy.Footprint.Offset, copy.Bytes);
                else
                {
                    const CD3DX12_TEXTURE_COPY_LOCATION source(payload.D3DResource.Get(), copy.Footprint);
                    const CD3DX12_TEXTURE_COPY_LOCATION target(copy.Region.Destination.Resource->D3DResource.Get(), copy.Region.DestinationSubresource);
                    commands.GetCommandList()->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
                }
        });
    return bytes;
}

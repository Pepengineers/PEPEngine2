#include "App.Base/App.h"
#include "Engine.RendererDX12/GDX12SnapshotTransport.h"
#include <d3d12sdklayers.h>
#include <array>
#include <chrono>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
using namespace GDX12Submission;
using namespace GDX12Snapshot;
using Graph = GDX12RenderGraph;
using Mode = Graph::Mode;

constexpr UINT64 BufferBytes = 256;

void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

class FixtureApp final : public App
{
public:
    FixtureApp() : App(GetModuleHandleW(nullptr)) {}
};

class Batch final : public GDX12RenderPass
{
public:
    using Recorder = std::function<void(GDX12CommandList&, const Graph::BoundResources&)>;

    Batch(const char* name, Placement location, std::vector<Graph::Access> accesses, Recorder record)
        : _name(name), _location(location), _accesses(std::move(accesses)), _record(std::move(record)) {}

    void RecordRenderGraph(GDX12RenderGraphBuilder& builder) override
    {
        builder.SetName(_name);
        builder.SetPlacement(_location);
        builder.DeclareCompleteAccesses();
        builder.HasSideEffects();
        for (const auto& access : _accesses)
            if (access.Version.IsValid()) builder.UseVersion(access.Id, access.Usage, access.Version, access.State);
            else builder.Use(access.Id, access.Usage, access.State);
    }

    bool SupportsGraphQueue(QueueKind kind) const override { return kind == _location.Queue; }
    void RecordPlacedCommands(GDX12CommandList* list, const Graph::BoundResources& views) override
    { _record(*list, views); }

private:
    const char* _name;
    Placement _location;
    std::vector<Graph::Access> _accesses;
    Recorder _record;
};

Batch CopyBatch(const char* name, Placement location, Graph::Resource source, Graph::Resource destination,
    TransferVersion input = {}, TransferVersion output = {})
{
    return Batch(name, location, {{source, Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE, input},
        {destination, Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST, output}},
        [=](GDX12CommandList& list, const Graph::BoundResources& views)
        { list.CopyResource(views.Get(destination), views.Get(source)); });
}

PhysicalView Buffer(GDX12Device& device, D3D12_HEAP_TYPE heap, RenderMGPURuntime& runtime, Epoch epoch,
    TransferId transfer = {})
{
    auto resource = std::make_shared<GDX12Resource>();
    const auto state = heap == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ :
        heap == D3D12_HEAP_TYPE_READBACK ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_COMMON;
    resource->D3DResource = device.CreateResource(heap, CD3DX12_RESOURCE_DESC::Buffer(BufferBytes), state);
    resource->SetCurrentState(state);
    return {std::move(resource), {runtime.NewResourceId(), device.GetSubmissionAdapterId(), epoch, transfer, runtime.NewAllocationId()}};
}

Graph::Resource BindView(Graph& graph, Graph::ExecutionBindings& bindings, const char* name,
    const PhysicalView& view, bool imported = false, bool shared = false)
{
    const auto key = imported ? graph.Import(name) : graph.Create(name);
    graph.RegisterResource(key, view.Identity, shared);
    bindings.Views.push_back({key, view.Identity, view.Resource});
    return key;
}

using Payload = std::array<std::uint32_t, 4>;

Payload Pattern(std::uint32_t sequence, Epoch epoch)
{
    return {sequence, static_cast<std::uint32_t>(epoch), sequence ^ 0x9e3779b9u, 0x50344750u};
}

void Fill(const PhysicalView& view, std::uint32_t sequence, Epoch epoch)
{
    const D3D12_RANGE noRead{0, 0};
    void* mapped = nullptr;
    ThrowIfFailed(view.Resource->D3DResource->Map(0, &noRead, &mapped));
    const auto pattern = Pattern(sequence, epoch);
    std::memset(mapped, 0, BufferBytes);
    std::memcpy(mapped, pattern.data(), sizeof(pattern));
    view.Resource->D3DResource->Unmap(0, nullptr);
}

void CheckPayload(const PhysicalView& view, std::uint32_t sequence, Epoch epoch)
{
    const D3D12_RANGE read{0, sizeof(Payload)}, noWrite{0, 0};
    void* mapped = nullptr;
    ThrowIfFailed(view.Resource->D3DResource->Map(0, &read, &mapped));
    Payload actual{};
    std::memcpy(actual.data(), mapped, sizeof(actual));
    view.Resource->D3DResource->Unmap(0, &noWrite);
    Require(actual == Pattern(sequence, epoch), "Snapshot sequence/epoch/checksum differs");
}

// An isolated fixture fence delays a queue without minting a submission token.
class Gate final
{
public:
    Gate(GDX12Device& device, GDX12CommandQueue& queue)
    {
        ThrowIfFailed(device.GetDevice()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_fence)));
        ThrowIfFailed(queue.GetCommandQueue()->Wait(_fence.Get(), 1));
    }
    ~Gate() { if (_fence) _fence->Signal(1); }
    void Open() { ThrowIfFailed(_fence->Signal(1)); _fence.Reset(); }

private:
    ComPtr<ID3D12Fence> _fence;
};

void WaitForFixture(GDX12CommandQueue& queue, CompletionPoint point)
{
    Require(point.IsValid() && point.Timeline() == queue.GetTimelineIdentity(), "Wrong fixture wait timeline");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (;;)
    {
        const auto progress = queue.GetCompletionProgress();
        if (point.IsReachedBy(progress)) return;
        if (progress.CompletedValue == UINT64_MAX) ThrowIfFailed(DXGI_ERROR_DEVICE_REMOVED);
        Require(std::chrono::steady_clock::now() < deadline, "Fixture completion timed out");
        Sleep(1);
    }
}

void CheckUnsubmittedRecorderFailure(GDX12Device& primary, RenderMGPURuntime& runtime)
{
    struct ExpectedFailure {};
    auto& direct = *primary.GetCommandQueue();
    const Placement directA{primary.GetSubmissionAdapterId(), QueueKind::Direct};
    const auto before = direct.GetLastCompletionPoint();
    Graph graph;
    Graph::ExecutionBindings bindings;
    const auto output = BindView(graph, bindings, "unsubmitted output",
        Buffer(primary, D3D12_HEAP_TYPE_DEFAULT, runtime, runtime.NewEpoch()));
    Batch invalid("failing recorder", directA, {{output, Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST, {}}},
        [](GDX12CommandList&, const Graph::BoundResources&) { throw ExpectedFailure{}; });
    graph.AddPass(invalid);
    graph.CompilePlan();
    bool failedBeforeSubmit = false;
    try
    {
        runtime.TrySubmit(graph, std::move(bindings), [](const auto&) { Require(false, "Failed recorder bound a receipt"); });
    }
    catch (const ExpectedFailure&) { failedBeforeSubmit = true; }
    const auto after = direct.GetLastCompletionPoint();
    Require(failedBeforeSubmit && !runtime.IsStopped() && runtime.PendingCount() == 0 &&
        before.Timeline() == after.Timeline() && before.Value() == after.Value(),
        "Recorder failure advanced DIRECT or stopped the runtime before native submit");
}

void CheckTerminalRetention()
{
    struct InitialRenderFailure {};
    std::weak_ptr<Transport> bankOwner;
    std::weak_ptr<GDX12Resource> slotOwner;
    RenderMGPURuntime* runtime = nullptr;
    try
    {
        FixtureApp app;
        Require(app.Initialize(), "Terminal fixture application startup failed");
        const auto render = BenchmarkEngine::GetLocator().GetModule<RenderModule>();
        const auto devices = render->GetMGPUDevices();
        runtime = render->GetMGPURuntime();
        Require(runtime && devices.Primary && devices.Secondary && devices.PrimaryCopy,
            "Terminal fixture needs two physical adapters");
        render->WaitForUploads();
        {
            const auto bank = runtime->CreateBank(runtime->NewTransferId(), runtime->NewEpoch(),
                devices.Primary->GetSubmissionAdapterId(), devices.Secondary->GetSubmissionAdapterId(),
                CD3DX12_RESOURCE_DESC::Buffer(BufferBytes));
            bankOwner = bank;
            slotOwner = bank->GetSlots().front().Export.Resource;
        }
        // Simulate a teardown failure without signaling a bogus point or inducing GPU removal.
        ++devices.PrimaryCopy->FenceValue;
        throw InitialRenderFailure{};
    }
    catch (const InitialRenderFailure&)
    {
        Require(!bankOwner.expired() && !slotOwner.expired() && runtime && runtime->IsStopped(),
            "Terminal shutdown freed GPU owners or left admission open");
        std::cout << "Terminal teardown retained the GPU domain and preserved the original render exception.\n";
    }
}

void Strict(const RenderMGPUDevices& devices, GDX12CommandQueue& compute, RenderMGPURuntime& runtime)
{
    auto& a = *devices.Primary;
    auto& b = *devices.Secondary;
    auto& direct = *a.GetCommandQueue();
    auto& copyA = *devices.PrimaryCopy;
    auto& copyB = *devices.SecondaryCopy;
    const Placement directA{a.GetSubmissionAdapterId(), QueueKind::Direct};
    const Placement copyOnA{a.GetSubmissionAdapterId(), QueueKind::Copy};
    const Placement copyOnB{b.GetSubmissionAdapterId(), QueueKind::Copy};
    const Placement computeB{b.GetSubmissionAdapterId(), QueueKind::Compute};
    const auto desc = CD3DX12_RESOURCE_DESC::Buffer(BufferBytes);
    const auto toB = runtime.NewTransferId(), toA = runtime.NewTransferId();
    const auto epoch = runtime.NewEpoch();
    auto outbound = runtime.CreateBank(toB, epoch, directA.Adapter, computeB.Adapter, desc);
    auto inbound = runtime.CreateBank(toA, epoch, computeB.Adapter, directA.Adapter, desc);
    const auto staleReader = compute.SignalCompletion();
    Gate holdCompute(b, compute);
    std::array<PhysicalView, 2> readback;
    CompletionPoint finished;

    try
    {
        for (std::uint32_t i = 0; i < readback.size(); ++i)
        {
            const ProducerInvocationId invocation{i + 1u};
            const TransferVersion input{toB, epoch, invocation}, output{toA, epoch, invocation};
            const auto upload = Buffer(a, D3D12_HEAP_TYPE_UPLOAD, runtime, epoch);
            const auto source = Buffer(a, D3D12_HEAP_TYPE_DEFAULT, runtime, epoch, toB);
            const auto result = Buffer(b, D3D12_HEAP_TYPE_DEFAULT, runtime, epoch, toA);
            readback[i] = Buffer(a, D3D12_HEAP_TYPE_READBACK, runtime, epoch);
            Fill(upload, i + 1, epoch);
            const auto send = outbound->Reserve(input, SnapshotReadMode::ExactVersion, direct,
                source.Resource->D3DResource, source.Identity.Allocation);
            const auto back = inbound->Reserve(output, SnapshotReadMode::ExactVersion, compute,
                result.Resource->D3DResource, result.Identity.Allocation);
            Require(send.Status == Result::Accepted && back.Status == Result::Accepted, "Exact slot reservation failed");
            Require(outbound->PrepareExact(send.Publication) && inbound->PrepareExact(back.Publication), "Exact preparation failed");
            const auto firstReader = outbound->PrepareExactReader(send.Publication, compute);
            const auto lastReader = inbound->PrepareExactReader(back.Publication, direct);
            Require(firstReader && lastReader, "Exact reader admission failed");

            Graph graph;
            Graph::ExecutionBindings bindings;
            bindings.KeepAlive = {outbound, inbound};
            const auto in = BindView(graph, bindings, "upload", upload, true);
            const auto src = BindView(graph, bindings, "source", source);
            const auto x0 = BindView(graph, bindings, "input export", outbound->GetSlots()[send.SharedSlot].Export, false, true);
            const auto x1 = BindView(graph, bindings, "input import", outbound->GetSlots()[send.SharedSlot].Import, false, true);
            const auto x2 = BindView(graph, bindings, "input local", firstReader->View);
            const auto solved = BindView(graph, bindings, "processed", result);
            const auto y0 = BindView(graph, bindings, "output export", inbound->GetSlots()[back.SharedSlot].Export, false, true);
            const auto y1 = BindView(graph, bindings, "output import", inbound->GetSlots()[back.SharedSlot].Import, false, true);
            const auto y2 = BindView(graph, bindings, "output local", lastReader->View);
            const auto out = BindView(graph, bindings, "readback", readback[i], true);

            auto produce = CopyBatch("primary producer", directA, in, src, {}, input);
            Batch exportInput("export input", copyOnA, {{src, Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE, input},
                {x0, Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST, input}},
                [&](GDX12CommandList& list, const Graph::BoundResources&)
                { outbound->RecordExportCopies(send.Publication, *list.GetCommandList().Get()); });
            Batch importInput("import input", copyOnB, {{x1, Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE, input},
                {x2, Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST, input}},
                [&](GDX12CommandList& list, const Graph::BoundResources&)
                { outbound->RecordImportCopies(send.Publication, *list.GetCommandList().Get()); });
            auto process = CopyBatch("secondary COMPUTE", computeB, x2, solved, input, output);
            Batch exportOutput("export output", copyOnB, {{solved, Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE, output},
                {y0, Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST, output}},
                [&](GDX12CommandList& list, const Graph::BoundResources&)
                { inbound->RecordExportCopies(back.Publication, *list.GetCommandList().Get()); });
            Batch importOutput("import output", copyOnA, {{y1, Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE, output},
                {y2, Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST, output}},
                [&](GDX12CommandList& list, const Graph::BoundResources&)
                { inbound->RecordImportCopies(back.Publication, *list.GetCommandList().Get()); });
            auto read = CopyBatch("primary reader", directA, y2, out, output);
            for (GDX12RenderPass* pass : {&produce, &exportInput, &importInput, &process, &exportOutput, &importOutput, &read})
                graph.AddPass(*pass);
            graph.CompilePlan();
            const auto receipt = runtime.TrySubmit(graph, std::move(bindings), [&](const Graph::ExecutionReceipt& submitted)
            {
                const auto& p = submitted.Batches;
                Require(p.size() == 7 && p[0].Timeline() == direct.GetTimelineIdentity() &&
                    p[1].Timeline() == copyA.GetTimelineIdentity() && p[2].Timeline() == copyB.GetTimelineIdentity() &&
                    p[3].Timeline() == compute.GetTimelineIdentity() && p[4].Timeline() == copyB.GetTimelineIdentity() &&
                    p[5].Timeline() == copyA.GetTimelineIdentity() && p[6].Timeline() == direct.GetTimelineIdentity(),
                    "Exact round trip receipt placement differs");
                outbound->CommitExact(send.Publication, p[0], p[1], p[1], p[2], p[2]);
                inbound->CommitExact(back.Publication, p[3], p[4], p[4], p[5], p[5]);
                Require(!outbound->BindReader(firstReader->Id, staleReader), "Reader accepted a point issued before admission");
                Require(outbound->BindReader(firstReader->Id, p[3]) && inbound->BindReader(lastReader->Id, p[6]),
                    "Same-plan reader receipts rejected");
                Require(outbound->ReleaseExact(send.Publication) && inbound->ReleaseExact(back.Publication), "Exact pin release failed");
                finished = p[6];
            });
            Require(receipt.has_value(), "Exact plan hit unexpected capacity backpressure");
        }
        Require(runtime.PendingCount() == 2, "Two strict invocations did not remain in flight");
        holdCompute.Open();
        WaitForFixture(direct, finished);
        runtime.Poll();
        CheckPayload(readback[0], 1, epoch); CheckPayload(readback[1], 2, epoch);
    }
    catch (...)
    {
        outbound->FaultSubmission(); inbound->FaultSubmission();
        throw;
    }
    std::cout << "Strict two-adapter P4 round trips: two in-flight frames, prepared reader provenance and payloads passed.\n";
}

void Background(const RenderMGPUDevices& devices, GDX12CommandQueue& compute, RenderMGPURuntime& runtime)
{
    auto& primary = *devices.Primary;
    auto& secondary = *devices.Secondary;
    auto& direct = *primary.GetCommandQueue();
    auto& exportQueue = *devices.SecondaryCopy;
    auto& importQueue = *devices.PrimaryCopy;
    const Placement directA{primary.GetSubmissionAdapterId(), QueueKind::Direct};
    const Placement computeB{secondary.GetSubmissionAdapterId(), QueueKind::Compute};
    const auto stream = runtime.NewTransferId();
    const auto epoch = runtime.NewEpoch();
    const auto bank = runtime.CreateBank(stream, epoch, computeB.Adapter, directA.Adapter,
        CD3DX12_RESOURCE_DESC::Buffer(BufferBytes));

    struct Producer
    {
        Reservation Slot;
        TransferVersion Version;
        PhysicalView Source;
        CompletionPoint Ready;
    };

    const auto produce = [&](std::uint32_t sequence)
    {
        const TransferVersion version{stream, epoch, ProducerInvocationId{sequence}};
        const auto upload = Buffer(secondary, D3D12_HEAP_TYPE_UPLOAD, runtime, epoch);
        const auto source = Buffer(secondary, D3D12_HEAP_TYPE_DEFAULT, runtime, epoch, stream);
        Fill(upload, sequence, epoch);
        const auto reserved = bank->Reserve(version, SnapshotReadMode::LatestCompleted, compute,
            source.Resource->D3DResource, source.Identity.Allocation);
        Require(reserved.Status == Result::Accepted, "Latest producer had no slot");
        Graph graph;
        Graph::ExecutionBindings bindings;
        bindings.KeepAlive = {bank};
        const auto in = BindView(graph, bindings, "producer upload", upload, true);
        const auto out = BindView(graph, bindings, "producer source", source);
        auto batch = CopyBatch("background COMPUTE producer", computeB, in, out, {}, version);
        graph.AddPass(batch);
        graph.CompilePlan();
        const auto receipt = runtime.TrySubmit(graph, std::move(bindings), [&](const Graph::ExecutionReceipt& submitted)
        { Require(bank->BindProducer(reserved.Publication, submitted.Batches.at(0)), "Latest producer receipt rejected"); });
        Require(receipt.has_value(), "Latest producer capacity exhausted");
        return Producer{reserved, version, source, receipt->Batches.front()};
    };

    const auto publish = [&](const Producer& producer)
    {
        WaitForFixture(compute, producer.Ready);
        const auto ticket = producer.Slot.Publication;
        const auto ready = bank->PrepareExport(ticket);
        Require(ready.has_value(), "Latest export preparation failed");
        const auto& shared = bank->GetSlots()[producer.Slot.SharedSlot];
        const auto version = producer.Version;
        {
            Graph graph;
            Graph::ExecutionBindings bindings;
            const auto source = BindView(graph, bindings, "completed producer", producer.Source, true);
            const auto destination = BindView(graph, bindings, "Latest export view", shared.Export, false, true);
            graph.ImportVersion(source, version);
            bindings.ExactInputs.push_back({source, version, *ready, bank});
            Batch exportBatch("Latest export", {computeB.Adapter, QueueKind::Copy},
                {{source, Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE, version},
                 {destination, Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST, version}},
                [&](GDX12CommandList& list, const Graph::BoundResources&)
                { bank->RecordExportCopies(ticket, *list.GetCommandList().Get()); });
            graph.AddPass(exportBatch);
            graph.CompilePlan();
            const auto exported = runtime.TrySubmit(graph, std::move(bindings), [&](const Graph::ExecutionReceipt& submitted)
            { bank->CommitExport(ticket, submitted.Batches.at(0), submitted.Batches.at(0)); });
            Require(exported.has_value(), "Latest export capacity exhausted");
            WaitForFixture(exportQueue, exported->Batches.front());
        }
        runtime.Poll();
        const auto import = bank->PrepareImport(ticket);
        Require(import.Status == Result::Accepted, "Completed Latest export did not admit import");
        {
            Graph graph;
            Graph::ExecutionBindings bindings;
            const auto source = BindView(graph, bindings, "Latest import view", shared.Import, true, true);
            const auto destination = BindView(graph, bindings, "Latest local view", bank->GetSlots()[import.LocalSlot].Local);
            graph.ImportVersion(source, version);
            bindings.ExactInputs.push_back({source, version, import.ExportReady, bank, &exportQueue, shared.Export.Identity});
            Batch importBatch("Latest import", {directA.Adapter, QueueKind::Copy},
                {{source, Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE, version},
                 {destination, Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST, version}},
                [&](GDX12CommandList& list, const Graph::BoundResources&)
                { bank->RecordImportCopies(ticket, *list.GetCommandList().Get()); });
            graph.AddPass(importBatch);
            graph.CompilePlan();
            const auto imported = runtime.TrySubmit(graph, std::move(bindings), [&](const Graph::ExecutionReceipt& submitted)
            { bank->CommitImport(ticket, submitted.Batches.at(0), submitted.Batches.at(0)); });
            Require(imported.has_value(), "Latest import capacity exhausted");
            WaitForFixture(importQueue, imported->Batches.front());
        }
        runtime.Poll();
    };

    try
    {
        publish(produce(1));
        const auto first = bank->SelectLatest(FrameSubmissionId{1});
        Require(first.Current.IsValid() && first.Current.Version.Invocation == ProducerInvocationId{1}, "Initial Latest result missing");
        Gate holdCompute(secondary, compute);
        const auto delayed = produce(2);
        for (std::uint64_t frame : {2u, 3u})
        {
            const auto selection = bank->SelectLatest(FrameSubmissionId{frame});
            Require(selection.Current.Publication == first.Current.Publication, "Pending producer replaced completed Latest result");
            const auto reader = bank->AcquireReader(selection.Current.Publication, direct);
            Require(reader.has_value(), "Latest reader admission failed");
            const auto readback = Buffer(primary, D3D12_HEAP_TYPE_READBACK, runtime, epoch);
            Graph graph;
            Graph::ExecutionBindings bindings;
            const auto in = BindView(graph, bindings, "selected completed snapshot", reader->View, true);
            const auto out = BindView(graph, bindings, "primary readback", readback, true);
            graph.ImportVersion(in, selection.Current.Version);
            bindings.ExactInputs.push_back({in, selection.Current.Version, selection.Current.ImportReady, bank});
            auto batch = CopyBatch("primary Latest frame", directA, in, out, selection.Current.Version);
            graph.AddPass(batch);
            graph.CompilePlan();
            const auto receipt = runtime.TrySubmit(graph, std::move(bindings), [&](const Graph::ExecutionReceipt& submitted)
            { Require(bank->BindReader(reader->Lease.Id, submitted.Batches.at(0)), "Latest reader release rejected"); });
            Require(receipt.has_value(), "Latest frame capacity exhausted");
            WaitForFixture(direct, receipt->Batches.front());
            Require(!delayed.Ready.IsReachedBy(compute.GetCompletionProgress()), "Background gate opened before primary frame");
            CheckPayload(readback, 1, epoch);
            runtime.Poll();
        }
        holdCompute.Open();
        publish(delayed);
        const auto advanced = bank->SelectLatest(FrameSubmissionId{4});
        Require(advanced.Current.Publication == delayed.Slot.Publication && advanced.Previous.Publication == first.Current.Publication,
            "Latest frame did not advance with a matching previous snapshot");
        const auto retiredIdentity = bank->GetSlots().front().Local.Identity;
        Require(bank->Reset(runtime.NewEpoch()) && !bank->SelectLatest(FrameSubmissionId{5}).Current.IsValid(),
            "Epoch reset did not invalidate the displayed snapshot");
        runtime.Poll();
        Require(runtime.TryReconfigureBank(*bank, CD3DX12_RESOURCE_DESC::Buffer(BufferBytes)),
            "Retired bank did not reconfigure");
        const auto activeIdentity = bank->GetSlots().front().Local.Identity;
        Require(activeIdentity.Transfer == retiredIdentity.Transfer && activeIdentity.ResourceEpoch != retiredIdentity.ResourceEpoch &&
            activeIdentity.Id != retiredIdentity.Id && activeIdentity.Allocation != retiredIdentity.Allocation,
            "Bank reconfiguration reused retired identities");
    }
    catch (...)
    {
        bank->FaultSubmission();
        throw;
    }
    std::cout << "Background Latest: two primary frames completed while producer was gated, then selected the new epoch-compatible result.\n";
}

void CheckDebug(GDX12Device& device)
{
    ComPtr<ID3D12InfoQueue> info;
    ThrowIfFailed(device.GetDevice().As(&info));
    for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i)
    {
        SIZE_T bytes = 0;
        ThrowIfFailed(info->GetMessage(i, nullptr, &bytes));
        std::vector<std::byte> storage(bytes);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        ThrowIfFailed(info->GetMessage(i, message, &bytes));
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) throw std::runtime_error(message->pDescription);
    }
}
}

int main(int argc, char** argv)
{
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    try
    {
        bool strict = false, missing = false, gbv = false, terminal = false;
        for (int i = 1; i < argc; ++i)
        {
            const std::string_view option = argv[i];
            if (option == "--two-adapters") strict = true;
            else if (option == "--missing-adapter") missing = true;
            else if (option == "--gpu-validation") gbv = true;
            else if (option == "--terminal-retention") terminal = true;
            else throw std::invalid_argument("Unknown MGPUIntegrationTests option");
        }
        Require(int(strict) + int(missing) + int(terminal) <= 1, "Choose one MGPU fixture mode");
        ComPtr<ID3D12Debug> debug;
        ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
        debug->EnableDebugLayer();
        if (gbv)
        {
            ComPtr<ID3D12Debug1> validation;
            ThrowIfFailed(debug.As(&validation));
            validation->SetEnableGPUBasedValidation(TRUE);
        }

        ConsoleModule{}.Initialize();
        if (terminal)
        {
            CheckTerminalRetention();
            return 0;
        }
        FixtureApp app;
        Require(app.Initialize(), "Fixture application startup failed");
        const auto render = BenchmarkEngine::GetLocator().GetModule<RenderModule>();
        const auto& status = render->GetMGPUStatus();
        const auto devices = render->GetMGPUDevices();
        Require(devices.Primary && status.Requested == (strict || missing), "Unexpected primary/bootstrap status");
        if (strict)
        {
            Require(status.SecondaryReady && devices.Secondary && devices.PrimaryCopy && devices.SecondaryCopy &&
                status.PrimaryLuid != status.ActiveSecondaryLuid, "Two physical adapters required for P4 fixture");
            auto* compute = devices.Secondary->TryGetComputeQueue();
            Require(compute, "Secondary COMPUTE queue unavailable");
            auto* runtime = render->GetMGPURuntime();
            Require(runtime, "RenderModule has no MGPU submission domain");
            bool wrongThreadRejected = false;
            std::thread foreign([&]
            {
                try { runtime->NewEpoch(); }
                catch (const std::logic_error&) { wrongThreadRejected = true; }
            });
            foreign.join();
            Require(wrongThreadRejected, "MGPU identity issued outside the render owner");
            CheckUnsubmittedRecorderFailure(*devices.Primary, *runtime);
            Strict(devices, *compute, *runtime);
            Background(devices, *compute, *runtime);
            CheckDebug(*devices.Secondary);
        }
        else
        {
            Require(!status.SecondaryReady && !devices.Secondary && !devices.PrimaryCopy && !devices.SecondaryCopy,
                "Default or unavailable request created a secondary submission domain");
            Require(!render->GetMGPURuntime(), "Primary fallback owns an MGPU domain");
            Require(!missing || !status.FallbackReason.empty(), "Missing-adapter fallback has no diagnostic");
            Require(render->GetRenderGraphStatistics().Passes != 0, "Primary fallback graph was not compiled");
            std::cout << (missing ? "Unavailable adapter primary fallback" : "Default-off primary graph") << " passed.\n";
        }
        CheckDebug(*devices.Primary);
        Require(render->ShutdownMGPU() && !render->GetMGPURuntime(), "Normal shutdown failed to retire the MGPU runtime");
        return 0;
    }
    catch (const DxException& error) { std::wcerr << error.ToString() << '\n'; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
    return 1;
}

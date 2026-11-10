#pragma once

#include <string_view>

// Include after namespace PlacedTests closes in RenderGraphTests.cpp.
namespace PlacedTests
{
static void RegistrationAndAccessIndex()
{
    Graph graph;
    const auto live = graph.Import("live registration");
    const auto existing = graph.Import("register during failed pass");
    const ResourceIdentity liveId{ResourceId{1}, AdapterId{1}, Epoch{1}, {}, AllocationId{1}};
    auto rolledBack = liveId;
    rolledBack.Id = ResourceId{2};
    auto newId = liveId;
    newId.Id = ResourceId{3};
    graph.RegisterResource(live, liveId);
    Reject([&] { graph.RegisterResource(existing, liveId); }, "Duplicate graph resource identity accepted");

    class FailingRegistration final : public GDX12RenderPass
    {
    public:
        FailingRegistration(Graph& graph, Graph::Resource existing, ResourceIdentity existingId, ResourceIdentity newId)
            : Work(graph), Existing(existing), ExistingId(existingId), NewId(newId) {}
        void RecordRenderGraph(GDX12RenderGraphBuilder&) override
        {
            Work.RegisterResource(Existing, ExistingId);
            Work.RegisterResource(Work.Import("new registration during failed pass"), NewId);
            throw std::runtime_error("Discard pass registrations");
        }
    private:
        Graph& Work;
        Graph::Resource Existing;
        ResourceIdentity ExistingId, NewId;
    } failure(graph, existing, rolledBack, newId);
    Reject([&] { graph.AddPass(failure); }, "Failed graph registration fixture did not throw");
    graph.RegisterResource(existing, rolledBack);
    const auto added = graph.Import("new registration after rollback");
    Require(added == existing + 1, "Failed pass left a graph resource behind");
    graph.RegisterResource(added, newId);

    const auto noop = [](GDX12CommandList&, const Graph::BoundResources&) {};
    const Placement lane{liveId.Adapter, QueueKind::Direct};
    const Graph::Access read{live, Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE};
    auto versioned = read;
    versioned.Version = {TransferId{1}, Epoch{1}, ProducerInvocationId{1}};
    Pass duplicate("duplicate version", lane, {versioned, versioned}, noop);
    Reject([&] { graph.AddPass(duplicate); }, "Duplicate versioned declaration accepted");
    auto conflicting = read;
    conflicting.State = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    Pass conflict("conflicting reads", lane, {read, conflicting}, noop);
    Reject([&] { graph.AddPass(conflict); }, "Incompatible read states coalesced");
    Pass coalesced("identical reads", lane, {read, read}, noop);
    graph.AddPass(coalesced);
    for (unsigned compile = 0; compile != 2; ++compile)
    {
        graph.CompilePlan();
        Require(graph.GetCompiledPlan().Batches.front().Accesses.size() == 1, "Identical reads no longer coalesce");
        Reject([&] { graph.RegisterResource(added, liveId); }, "Recompile lost a registered identity");
    }
    Graph fresh;
    fresh.RegisterResource(fresh.Import("independent graph"), liveId);
}

static void IndexedViewCollisions(GDX12Device& device)
{
    auto& direct = *device.GetCommandQueue();
    const Placement lane{device.GetSubmissionAdapterId(), QueueKind::Direct};
    auto backing = MakeResource(device, D3D12_HEAP_TYPE_DEFAULT);
    auto other = MakeResource(device, D3D12_HEAP_TYPE_DEFAULT);
    Graph::PlanExecutor executor(2);
    const auto noop = [](GDX12CommandList&, const Graph::BoundResources&) {};
    const auto reject = [&](std::shared_ptr<GDX12Resource> candidate, uint64_t id, uint64_t allocation,
        bool current, std::string_view expected)
    {
        Graph graph;
        Graph::ExecutionBindings bindings;
        bindings.Queues = {{&device, &direct}};
        std::vector<Graph::Access> reads;
        if (current)
        {
            const auto first = Register(graph, bindings, "first backing", device, backing, 1, 1, {}, true);
            reads.push_back({first, Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE});
        }
        const auto second = Register(graph, bindings, "alias candidate", device, std::move(candidate), id, allocation, {}, true);
        reads.push_back({second, Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE});
        Pass pass("view collision", lane, std::move(reads), noop);
        graph.AddPass(pass);
        graph.CompilePlan();
        const auto before = direct.GetLastCompletionPoint().Value();
        bool matched = false;
        try { executor.TrySubmit(graph, bindings); }
        catch (const std::invalid_argument& error) { matched = std::string_view(error.what()) == expected; }
        Require(matched && direct.GetLastCompletionPoint().Value() == before, "Alias preflight did not reject the expected collision");
    };
    constexpr auto Identity = "Graph executor: inconsistent native view identity";
    constexpr auto Allocation = "Graph executor: one native view per device/backing is required";
    reject(backing, 2, 2, true, Identity);
    reject(other, 2, 1, true, Allocation);

    Graph producer;
    Graph::ExecutionBindings bindings;
    bindings.Queues = {{&device, &direct}};
    const auto resource = Register(producer, bindings, "pending backing", device, backing, 1, 1, {}, true);
    Pass hold("pending view", lane, {{resource, Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE}}, noop);
    producer.AddPass(hold);
    producer.CompilePlan();
    ComPtr<ID3D12Fence> fence;
    Gate gate(device, direct, fence);
    const auto pending = executor.TrySubmit(producer, bindings);
    Require(pending && !pending->Batches.front().IsReachedBy(direct.GetCompletionProgress()), "Alias fixture needs a pending view");
    reject(other, 1, 2, false, Identity);
    reject(backing, 2, 2, false, Identity);
    reject(other, 2, 1, false, Allocation);
    gate.Open();
    executor.Drain();
}

static void IndexedReaderOrdering(GDX12Device& device)
{
    auto& direct = *device.GetCommandQueue();
    auto* compute = device.TryGetComputeQueue();
    Require(compute != nullptr, "Indexed hazard fixture needs COMPUTE");
    const auto adapter = device.GetSubmissionAdapterId();
    const auto noop = [](GDX12CommandList&, const Graph::BoundResources&) {};
    enum class Scenario { InPlanRead, PendingRead, PendingOverwrite };
    for (auto scenario : {Scenario::InPlanRead, Scenario::PendingRead, Scenario::PendingOverwrite})
    {
        const bool onePlan = scenario == Scenario::InPlanRead;
        const bool overwrite = scenario == Scenario::PendingOverwrite;
        auto data = MakeResource(device, D3D12_HEAP_TYPE_DEFAULT);
        auto seed = MakeResource(device, D3D12_HEAP_TYPE_UPLOAD);
        auto resultA = MakeResource(device, D3D12_HEAP_TYPE_READBACK);
        auto resultB = MakeResource(device, D3D12_HEAP_TYPE_READBACK);
        Fill(*seed, 17);
        Graph::PlanExecutor executor(3);
        {
            Graph init;
            Graph::ExecutionBindings bindings;
            bindings.Queues = {{&device, &direct}};
            const auto from = Register(init, bindings, "seed", device, seed, 1, 0, {}, true);
            const auto to = Register(init, bindings, "data", device, data, 2, 0);
            auto initialize = Copy("initialize data", {adapter, QueueKind::Direct}, from, to);
            init.AddPass(initialize);
            init.CompilePlan();
            Require(executor.TrySubmit(init, bindings).has_value(), "Could not initialize hazard fixture");
            executor.Drain();
        }
        if (overwrite) Fill(*seed, 23);

        Graph firstGraph, nextGraph;
        Graph::ExecutionBindings firstBindings, nextBindings;
        firstBindings.Queues = {{&device, compute}};
        nextBindings.Queues = {{&device, &direct}};
        if (onePlan) firstBindings.Queues.push_back({&device, &direct});
        auto& consumerGraph = onePlan ? firstGraph : nextGraph;
        auto& consumerBindings = onePlan ? firstBindings : nextBindings;
        const auto firstView = Register(firstGraph, firstBindings, "first shared view", device, data, 2, 0, {}, true);
        const auto secondView = onePlan ? firstView :
            Register(nextGraph, nextBindings, "second shared view", device, data, 2, 0, {}, true);
        const auto outA = Register(firstGraph, firstBindings, "first readback", device, resultA, 3, 0, {}, true);
        const auto other = Register(consumerGraph, consumerBindings, "other endpoint", device,
            overwrite ? seed : resultB, 4, 0, {}, true);
        Pass prefix("independent DIRECT", {adapter, QueueKind::Direct}, {}, noop);
        auto reader = Copy("gated COMPUTE read", {adapter, QueueKind::Compute}, firstView, outA);
        auto consumer = overwrite ? Copy("DIRECT overwrite", {adapter, QueueKind::Direct}, other, secondView)
            : Copy("DIRECT read", {adapter, QueueKind::Direct}, secondView, other);
        if (onePlan) firstGraph.AddPass(prefix);
        firstGraph.AddPass(reader);
        consumerGraph.AddPass(consumer);
        firstGraph.CompilePlan();
        if (!onePlan) nextGraph.CompilePlan();

        ComPtr<ID3D12Fence> fence;
        Gate gate(device, *compute, fence);
        const auto independent = onePlan ? CompletionPoint{} : direct.SignalCompletion();
        const auto issued = executor.TrySubmit(firstGraph, firstBindings);
        Require(issued.has_value(), "Gated read did not issue");
        CompletionPoint consumerPoint;
        if (onePlan) consumerPoint = issued->Batches.back();
        else
        {
            const auto next = executor.TrySubmit(nextGraph, nextBindings);
            Require(next.has_value(), "Pending hazard prevented consumer issue");
            consumerPoint = next->Batches.back();
        }
        RequireReached(onePlan ? issued->Batches.front() : independent, direct);
        Require(!consumerPoint.IsReachedBy(direct.GetCompletionProgress()), "COMMON reader or pending overwrite bypassed COMPUTE");
        gate.Open();
        executor.Drain();
        CheckPayload(*resultA, 17);
        if (!overwrite) CheckPayload(*resultB, 17);
    }
}

static void IndexedExactWriter(GDX12Device& device)
{
    auto& direct = *device.GetCommandQueue();
    auto* compute = device.TryGetComputeQueue();
    Require(compute != nullptr, "Indexed Exact fixture needs COMPUTE");
    const auto adapter = device.GetSubmissionAdapterId();
    const TransferVersion first{TransferId{1}, Epoch{1}, ProducerInvocationId{1}};
    const TransferVersion last{TransferId{1}, Epoch{1}, ProducerInvocationId{2}};
    auto olderSeed = MakeResource(device, D3D12_HEAP_TYPE_UPLOAD);
    auto newerSeed = MakeResource(device, D3D12_HEAP_TYPE_UPLOAD);
    auto data = MakeResource(device, D3D12_HEAP_TYPE_DEFAULT);
    auto output = MakeResource(device, D3D12_HEAP_TYPE_READBACK);
    Fill(*olderSeed, 31);
    Fill(*newerSeed, 37);

    Graph graph;
    Graph::ExecutionBindings bindings;
    bindings.Queues = {{&device, compute}};
    const auto fromOlder = Register(graph, bindings, "older seed", device, olderSeed, 1, 1, {}, true);
    const auto to = Register(graph, bindings, "written version", device, data, 2, 2, first);
    const auto fromNewer = Register(graph, bindings, "newer seed", device, newerSeed, 4, 4, {}, true);
    auto older = Copy("older writer", {adapter, QueueKind::Compute}, fromOlder, to, {}, first);
    auto newer = Copy("newer writer", {adapter, QueueKind::Compute}, fromNewer, to, {}, last);
    graph.AddPass(older);
    graph.AddPass(newer);
    graph.CompilePlan();

    Graph::PlanExecutor executor(2);
    ComPtr<ID3D12Fence> fence;
    Gate gate(device, *compute, fence);
    const auto prefix = direct.SignalCompletion();
    const auto issued = executor.TrySubmit(graph, bindings);
    Require(issued && issued->Batches.size() == 2, "Exact writer fixture did not issue both versions");
    const auto read = [&](TransferVersion version, CompletionPoint ready)
    {
        Graph consumer;
        Graph::ExecutionBindings input;
        input.Queues = {{&device, &direct}};
        const auto view = Register(consumer, input, "imported version", device, data, 2, 2, version, true);
        consumer.ImportVersion(view, version);
        input.ExactInputs.push_back({view, version, ready, data});
        const auto result = Register(consumer, input, "readback", device, output, 3, 3, {}, true);
        auto copy = Copy("read pending exact writer", {adapter, QueueKind::Direct}, view, result, version);
        consumer.AddPass(copy);
        consumer.CompilePlan();
        return executor.TrySubmit(consumer, input);
    };
    Reject([&] { read(first, issued->Batches.front()); }, "Older Exact input ignored the later pending writer");
    Require(direct.GetLastCompletionPoint().Value() == prefix.Value(), "Conflicting Exact input advanced DIRECT");
    const auto submitted = read(last, issued->Batches.back());
    Require(submitted.has_value(), "Latest pending writer was rejected");
    RequireReached(prefix, direct);
    Require(!submitted->Batches.back().IsReachedBy(direct.GetCompletionProgress()), "Pending writer read bypassed COMPUTE");
    gate.Open();
    executor.Drain();
    CheckPayload(*output, 37);
}

static void DistinctPlanViews(GDX12Device& device)
{
    auto& direct = *device.GetCommandQueue();
    auto* compute = device.TryGetComputeQueue();
    Require(compute != nullptr, "Large placed graph fixture needs COMPUTE");
    const auto adapter = device.GetSubmissionAdapterId();
    Graph graph;
    Graph::ExecutionBindings bindings;
    bindings.Queues = {{&device, &direct}, {&device, compute}};
    std::vector<Graph::Access> directReads, computeReads;
    constexpr unsigned Count = 1024;
    for (unsigned i = 0; i < Count; ++i)
    {
        const uint64_t identity = i + 1;
        const auto view = Register(graph, bindings, "distinct buffer", device,
            MakeResource(device, D3D12_HEAP_TYPE_DEFAULT), identity, identity, {}, true);
        auto& accesses = i < Count / 2 ? directReads : computeReads;
        accesses.push_back({view, Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE});
    }
    const auto noop = [](GDX12CommandList&, const Graph::BoundResources&) {};
    Pass first("large distinct DIRECT reads", {adapter, QueueKind::Direct}, std::move(directReads), noop);
    Pass second("large distinct COMPUTE reads", {adapter, QueueKind::Compute}, std::move(computeReads), noop);
    graph.AddPass(first);
    graph.AddPass(second);
    graph.CompilePlan();
    Graph::PlanExecutor executor(1);
    const auto receipt = executor.TrySubmit(graph, bindings);
    Require(receipt && receipt->Batches.size() == 2 && graph.GetCompiledPlan().Views.size() == Count,
        "Large distinct placed view plan did not issue both batches");
    executor.Drain();
}

static void PlanScaleChecks(GDX12Device& device)
{
    RegistrationAndAccessIndex();
    IndexedViewCollisions(device);
    IndexedReaderOrdering(device);
    IndexedExactWriter(device);
    DistinctPlanViews(device);
}
}

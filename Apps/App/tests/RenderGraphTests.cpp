#include "App.Base/App.h"
#include "Engine.RendererDX12/GDX12RenderGraph.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include "Engine.RendererDX12/GDX12StreamlineSDK.h"
#include <d3d12sdklayers.h>
#include <iostream>
#include <sstream>
#include "Common/GameTimer.h"
#include <cmath>
#include <stdexcept>

using Graph = GDX12RenderGraph;
using Mode = Graph::Mode;
static void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> static void Reject(F action, const char* message)
{
    bool rejected = false;
    try { action(); } catch (const std::exception&) { rejected = true; }
    Require(rejected, message);
}
class TestPass : public GDX12RenderPass
{
public:
    TestPass(std::string name, std::vector<Graph::Access> accesses, bool sideEffect = false)
        : Name(std::move(name)), Accesses(std::move(accesses)), SideEffect(sideEffect) {}
    void RecordRenderGraph(GDX12RenderGraphBuilder& graph) override
    {
        graph.SetName(Name);
        for (const auto& access : Accesses) graph.Use(access.Id, access.Usage, access.State);
        if (SideEffect) graph.HasSideEffects();
        if (Async) graph.AsyncCompute();
    }
    bool Async = false;
private:
    std::string Name;
    std::vector<Graph::Access> Accesses;
    bool SideEffect;
};

class ClearPass final : public TestPass
{
public:
    ClearPass(const char* name, Graph::Resource resource, GDX12Texture& texture, std::array<float, 4> color)
        : TestPass(name, {{ resource, Mode::Write, D3D12_RESOURCE_STATE_RENDER_TARGET }}), Texture(texture), Color(color) {}
    void Execute(GDX12CommandList* commands) override
    { commands->GetCommandList()->ClearRenderTargetView(Texture.GetRTV()->CPUHandle, Color.data(), 0, nullptr); }
    void SetRed(float red) { Color[0] = red; }
private:
    GDX12Texture& Texture;
    std::array<float, 4> Color;
};

class CopyPass final : public TestPass
{
public:
    CopyPass(const char* name, Graph::Resource resource, Graph::Resource output, GDX12Texture& texture,
        ID3D12Resource* readback, D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint)
        : TestPass(name, {{ resource, Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE }, { output, Mode::Write }}),
          Texture(texture), Readback(readback), Footprint(footprint) {}
    void Execute(GDX12CommandList* commands) override
    {
        commands->ResourceBarrier({ Texture.GetResource()->GetCopySourceBarrier() });
        CD3DX12_TEXTURE_COPY_LOCATION source(Texture.GetResource()->D3DResource.Get(), 0);
        CD3DX12_TEXTURE_COPY_LOCATION destination(Readback, Footprint);
        commands->GetCommandList()->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    }
    void SetOffset(UINT64 offset) { Footprint.Offset = offset; }
private:
    GDX12Texture& Texture;
    ID3D12Resource* Readback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT Footprint;
};

static void TestCaptureClock()
{
    GameTimer timer; timer.Reset();
    timer.Tick(1.0 / 60, 2.0);
    Require(std::abs(timer.TotalTime() - 2.0f) < 1e-6f && std::abs(timer.DeltaTime() - 1.0f / 60) < 1e-7f,
        "Fixed capture clock did not expose the requested delta and elapsed time");
    timer.Tick();
    Require(timer.TotalTime() >= 0 && timer.TotalTime() < 1 && timer.DeltaTime() >= 0 && timer.DeltaTime() < 1,
        "Ordinary Tick inherited the capture's synthetic clock");
    timer.Stop(); const float stopped = timer.TotalTime(); timer.Tick(1.0 / 60, 3.0);
    Require(timer.DeltaTime() == 0 && timer.TotalTime() == stopped, "Paused capture clock advanced");
    timer.Start(); timer.Tick(1.0 / 60, 4.0);
    Require(std::abs(timer.TotalTime() - 4.0f) < 1e-6f, "Capture clock did not resume");
    timer.Reset(); Require(timer.TotalTime() == 0 && timer.DeltaTime() == 0, "Clock Reset retained synthetic time");
    Reject([&] { timer.Tick(1.0 / 60, -1); }, "Negative capture time accepted");
    Reject([&] { timer.Tick(-1, 0); }, "Negative capture delta accepted");
}

static void TestCompiler()
{
    GDX12Resource states;
    const auto readUnion = D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    states.SetCurrentState(readUnion);
    states.BeginConcurrentRead();
    states.GetDepthReadBarrier();
    states.GetNonPixelShaderResourceBarrier();
    states.GetSRVBarrier();
    Require(states.GetCurrentState() == readUnion, "Concurrent reader narrowed the published union");
    Reject([&] { states.GetDepthWriteBarrier(); }, "Concurrent texture writer accepted");
    Reject([&] { states.GetCommonBarrier(); }, "Concurrent texture layout release accepted");
    Reject([&] { states.SetCurrentState(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE); }, "Concurrent state override accepted");
    Reject([&] { states.GetUAVBarrier(); }, "Concurrent UAV barrier accepted");
    states.EndConcurrentRead();
    states.GetDepthWriteBarrier();
    Require(states.GetCurrentState() == D3D12_RESOURCE_STATE_DEPTH_WRITE, "Join did not release the published state");

    Graph graph;
    const auto input = graph.Import("input"), temporary = graph.Create("temporary"), dead = graph.Create("unused");
    const auto output = graph.Import("present");
    TestPass unused("unused", {{ dead, Mode::Write }});
    TestPass produce("producer", {{ input, Mode::Read }, { temporary, Mode::Write }});
    TestPass consume("consumer", {{ temporary, Mode::Read }, { output, Mode::Write }});
    graph.AddPass(unused);
    const auto producer = graph.AddPass(produce);
    const auto consumer = graph.AddPass(consume);
    graph.Compile();
    Require(graph.GetExecutionOrder() == std::vector<Graph::Pass>{ producer, consumer }, "Dependency scheduling or dead-pass culling failed");
    Require(graph.GetStatistics().CulledPasses == 1, "Unused pass was not culled");
    graph.DependsOn(producer, consumer);
    Reject([&] { graph.Compile(); }, "Dependency cycle accepted");
    TestPass duplicate("duplicate", {{ input, Mode::Read }, { input, Mode::Write }});
    Reject([&] { graph.AddPass(duplicate); }, "Duplicate access accepted");
    Reject([&] { graph.AddPass(produce); }, "Duplicate pass instance accepted");
    Reject([&] { graph.Export(UINT_MAX); }, "Invalid resource accepted");

    Graph missing;
    const auto absent = missing.Create("not initialized");
    TestPass reader("reader", {{ absent, Mode::ReadWrite }}, true);
    missing.AddPass(reader);
    Reject([&] { missing.Compile(); }, "Read-before-write accepted");

    Graph hazards;
    const auto shared = hazards.Import("shared");
    TestPass readPass("read", {{ shared, Mode::Read }});
    TestPass overwritePass("overwrite", {{ shared, Mode::Write }});
    TestPass updatePass("update", {{ shared, Mode::ReadWrite }});
    const auto read = hazards.AddPass(readPass);
    const auto overwrite = hazards.AddPass(overwritePass);
    const auto update = hazards.AddPass(updatePass);
    hazards.Compile();
    Require(hazards.GetExecutionOrder() == std::vector<Graph::Pass>{ read, overwrite, update }, "WAR/WAW dependency lost");
    const auto fallback = hazards.Create("forwarded");
    hazards.AddFallback(fallback, shared);
    Reject([&] { hazards.AddFallback(shared, fallback); }, "Fallback cycle accepted");
    std::cout << "Graph scheduling, hazards, culling, invalid accesses and cycles passed.\n";
}

namespace PlanTests
{
using namespace GDX12Submission;
constexpr AdapterId GPU0{ 1 }, GPU1{ 2 };
constexpr TransferId InputStream{ 1 }, OutputStream{ 2 };
constexpr TransferVersion InputVersion{ InputStream, Epoch{ 1 }, ProducerInvocationId{ 7 } };
constexpr TransferVersion OutputVersion{ OutputStream, Epoch{ 1 }, ProducerInvocationId{ 7 } };

class Pass final : public GDX12RenderPass
{
public:
    Pass(const char* name, Placement location, std::vector<Graph::Access> accesses = {}, bool sideEffect = false)
        : Name(name), Location(location), Accesses(std::move(accesses)), SideEffect(sideEffect) {}
    void RecordRenderGraph(GDX12RenderGraphBuilder& builder) override
    {
        builder.SetName(Name);
        builder.SetPlacement(Location);
        if (Complete) builder.DeclareCompleteAccesses();
        for (const auto& access : Accesses)
            if (access.Version.IsValid()) builder.UseVersion(access.Id, access.Usage, access.Version, access.State);
            else builder.Use(access.Id, access.Usage, access.State);
        if (SideEffect) builder.HasSideEffects();
    }
    bool SupportsGraphQueue(QueueKind) const override { return Supported; }
    std::vector<Graph::BufferAccess> GetBufferAccesses() const override
    { return HiddenBuffer ? std::vector<Graph::BufferAccess>{ { nullptr, Mode::Write } } : std::vector<Graph::BufferAccess>{}; }
    bool Complete = true, Supported = true, HiddenBuffer = false;
private:
    const char* Name;
    Placement Location;
    std::vector<Graph::Access> Accesses;
    bool SideEffect;
};

static Graph::Resource View(Graph& graph, const char* name, AdapterId adapter, AllocationId allocation,
    TransferId stream = TransferId::Invalid, bool imported = false, bool crossAdapter = false, Epoch epoch = Epoch{1})
{
    const auto r = imported ? graph.Import(name) : graph.Create(name);
    graph.RegisterResource(r, { ResourceId{ r + 1ull }, adapter, epoch, stream, allocation }, crossAdapter);
    return r;
}

static bool Edge(const Graph::CompiledPlan& plan, Graph::Pass producer, Graph::Pass consumer)
{
    auto a = Graph::PlannedBatchId::Invalid, b = Graph::PlannedBatchId::Invalid;
    for (const auto& batch : plan.Batches)
        for (auto pass : batch.Passes)
        {
            if (pass == producer) a = batch.Id;
            if (pass == consumer) b = batch.Id;
        }
    for (const auto& edge : plan.Dependencies) if (edge.Producer == a && edge.Consumer == b) return true;
    return false;
}

static void HAO()
{
    Graph graph;
    const auto depth0 = View(graph, "depth0", GPU0, AllocationId{ 1 }, InputStream);
    const auto inputShared0 = View(graph, "inputShared0", GPU0, AllocationId{ 2 }, InputStream, false, true);
    const auto inputShared1 = View(graph, "inputShared1", GPU1, AllocationId{ 2 }, InputStream, false, true);
    const auto depth1 = View(graph, "depth1", GPU1, AllocationId{ 3 }, InputStream);
    const auto ao1 = View(graph, "ao1", GPU1, AllocationId{ 4 }, OutputStream);
    const auto outputShared1 = View(graph, "outputShared1", GPU1, AllocationId{ 5 }, OutputStream, false, true);
    const auto outputShared0 = View(graph, "outputShared0", GPU0, AllocationId{ 5 }, OutputStream, false, true);
    const auto ao0 = View(graph, "ao0", GPU0, AllocationId{ 6 }, OutputStream);
    Pass depth("depth", {GPU0, QueueKind::Direct}, {{depth0, Mode::Write, D3D12_RESOURCE_STATE_RENDER_TARGET, InputVersion}});
    Pass exportInput("export input", {GPU0, QueueKind::Copy}, {
        {depth0, Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE, InputVersion},
        {inputShared0, Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST, InputVersion}});
    Pass importInput("import input", {GPU1, QueueKind::Copy}, {
        {inputShared1, Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE, InputVersion},
        {depth1, Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST, InputVersion}});
    Pass ao("AO", {GPU1, QueueKind::Compute}, {
        {depth1, Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, InputVersion},
        {ao1, Mode::Write, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, OutputVersion}});
    Pass exportAO("export AO", {GPU1, QueueKind::Copy}, {
        {ao1, Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE, OutputVersion},
        {outputShared1, Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST, OutputVersion}});
    Pass importAO("import AO", {GPU0, QueueKind::Copy}, {
        {outputShared0, Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE, OutputVersion},
        {ao0, Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST, OutputVersion}});
    Pass lighting("lighting", {GPU0, QueueKind::Direct}, {{ao0, Mode::Read, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, OutputVersion}}, true);
    graph.AddPass(depth); graph.AddPass(exportInput); graph.AddPass(importInput); graph.AddPass(ao);
    graph.AddPass(exportAO); graph.AddPass(importAO); graph.AddPass(lighting);
    graph.CompilePlan();
    const auto first = graph.GetCompiledPlan();
    Require(first.Batches.size() == 7 && first.Inputs.empty() && first.RequiresNewExecutor, "HAO plan lost work or fabricated external inputs");
    for (Graph::Pass p = 1; p < 7; ++p) Require(Edge(first, p - 1, p), "HAO lost exact producer dependency");
    Require(first.Views[inputShared0].Identity.Allocation == first.Views[inputShared1].Identity.Allocation &&
        first.Views[inputShared0].Identity.Id != first.Views[inputShared1].Identity.Id, "Shared views lost physical alias identity");
    graph.CompilePlan();
    const auto& second = graph.GetCompiledPlan();
    Require(first.Dependencies.size() == second.Dependencies.size(), "Nondeterministic dependency count");
    for (size_t i = 0; i < first.Dependencies.size(); ++i)
        Require(first.Dependencies[i].Producer == second.Dependencies[i].Producer &&
            first.Dependencies[i].Consumer == second.Dependencies[i].Consumer, "Nondeterministic batch dependencies");
    for (size_t i = 0; i < first.Batches.size(); ++i)
        Require(first.Batches[i].Id == second.Batches[i].Id && first.Batches[i].Location == second.Batches[i].Location &&
            first.Batches[i].Passes == second.Batches[i].Passes, "Nondeterministic batch placement/order");
    std::ostringstream dot;
    graph.WriteDot(dot);
    Require(dot.str().find("adapter=2 COMPUTE") != std::string::npos && dot.str().find("COPY") != std::string::npos,
        "Placement diagnostics mislabeled explicit queues");
    Reject([&] { graph.Execute(nullptr); }, "Legacy executor silently accepted COPY/cross-device plan");
}

static void IndependentProducer()
{
    // Two invocations of the same template share no frame counter or completion value.
    for (unsigned invocation : { 7u, 19u })
    {
        const TransferVersion version{ InputStream, Epoch{1}, ProducerInvocationId{invocation} };
        Graph graph;
        const auto input = View(graph, "input", GPU1, AllocationId{1}, InputStream, true);
        const auto output = View(graph, "output", GPU1, AllocationId{2}, InputStream);
        graph.ImportVersion(input, version);
        Pass producer("independent producer", {GPU1, QueueKind::Compute}, {
            {input, Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, version},
            {output, Mode::Write, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, version}});
        Pass frame("frame", {GPU0, QueueKind::Direct}, {}, true);
        const auto producerId = graph.AddPass(producer), frameId = graph.AddPass(frame);
        graph.Export(output);
        graph.CompilePlan();
        const auto& plan = graph.GetCompiledPlan();
        Require(plan.Batches.size() == 2 && plan.Dependencies.empty() && !Edge(plan, producerId, frameId),
            "Independent producer acquired a frame/Present join");
        Require(plan.Inputs.size() == 1 && plan.Inputs[0].View == input && plan.Inputs[0].Version == version &&
            plan.Inputs[0].Consumer == Graph::PlannedBatchId{0}, "External exact input lost invocation/provenance");
    }
}

static void ExternalProvenance()
{
    Graph graph;
    const auto input0 = View(graph, "input0", GPU0, AllocationId{1}, InputStream, true);
    const auto input1 = View(graph, "input1", GPU1, AllocationId{2}, OutputStream, true);
    graph.ImportVersion(input0, InputVersion); graph.ImportVersion(input1, OutputVersion);
    Pass read0("read0", {GPU0, QueueKind::Direct}, {{input0, Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, InputVersion}}, true);
    Pass read1("read1", {GPU1, QueueKind::Compute}, {{input1, Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, OutputVersion}}, true);
    graph.AddPass(read0); graph.AddPass(read1); graph.CompilePlan();
    Require(graph.GetCompiledPlan().Inputs.size() == 2 && graph.GetCompiledPlan().Dependencies.empty(),
        "Equal invocation numbers on different streams fabricated a producer edge");
    Reject([&] { graph.ImportVersion(input0, OutputVersion); }, "Wrong-stream import accepted");
    Reject([&] { graph.ImportVersion(input0, {}); }, "Invalid external version accepted");
    Reject([&] { graph.Execute(nullptr); }, "Unbound external readiness reached legacy submission");
}

static void AliasesAndFIFO()
{
    Graph graph;
    const auto a = View(graph, "stream A local", GPU0, AllocationId{100}, InputStream);
    const auto b = View(graph, "stream A imported view", GPU1, AllocationId{100}, InputStream, false, true);
    const auto c = View(graph, "different stream, same allocation", GPU0, AllocationId{100}, OutputStream, false, false, Epoch{2});
    const TransferVersion replacement{OutputStream, Epoch{2}, ProducerInvocationId{3}};
    Pass writer("writer", {GPU0, QueueKind::Direct}, {{a, Mode::Write, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, InputVersion}});
    Pass reader("reader", {GPU1, QueueKind::Compute}, {{b, Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, InputVersion}}, true);
    Pass overwrite("overwrite", {GPU0, QueueKind::Copy}, {{c, Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST, replacement}}, true);
    const auto w = graph.AddPass(writer), r = graph.AddPass(reader), o = graph.AddPass(overwrite);
    graph.CompilePlan();
    Require(Edge(graph.GetCompiledPlan(), w, r) && Edge(graph.GetCompiledPlan(), r, o) && Edge(graph.GetCompiledPlan(), w, o),
        "Allocation RAW/WAR/WAW lost across device views or streams");
    graph.DependsOn(w, o);
    Reject([&] { graph.CompilePlan(); }, "Alias dependency cycle accepted");
    Reject([&] { graph.GetCompiledPlan(); }, "Failed compile exposed stale plan");

    Graph fifo;
    Pass first("first copy", {GPU0, QueueKind::Copy}, {}, true);
    Pass middle("compute", {GPU1, QueueKind::Compute}, {}, true);
    Pass last("last copy", {GPU0, QueueKind::Copy}, {}, true);
    const auto f = fifo.AddPass(first), m = fifo.AddPass(middle), l = fifo.AddPass(last);
    fifo.DependsOn(f, m); fifo.DependsOn(m, l); // pass DAG l -> m -> f, FIFO f -> l
    Reject([&] { fifo.CompilePlan(); }, "FIFO-induced wait cycle accepted");
    Graph backward;
    Pass early("early", {GPU0, QueueKind::Copy}, {}, true), late("late", {GPU0, QueueKind::Copy}, {}, true);
    const auto e = backward.AddPass(early), t = backward.AddPass(late);
    backward.DependsOn(e, t);
    Reject([&] { backward.CompilePlan(); }, "Backward same-queue wait accepted");

    // Same queue kind on different adapters does not create a FIFO edge.
    Graph independent;
    Pass one("GPU0 copy", {GPU0, QueueKind::Copy}, {}, true), two("GPU1 copy", {GPU1, QueueKind::Copy}, {}, true);
    const auto oneId = independent.AddPass(one), twoId = independent.AddPass(two); independent.CompilePlan();
    Require(independent.GetCompiledPlan().Dependencies.empty(), "QueueKind alone merged distinct device timelines");
    independent.DependsOn(twoId, oneId); independent.CompilePlan();
    Require(Edge(independent.GetCompiledPlan(), oneId, twoId), "Explicit cross-queue dependency was lost");
}

static void InvalidContracts()
{
    Graph identities;
    const auto a = identities.Create("a"), b = identities.Create("b");
    const ResourceIdentity identity{ResourceId{1}, GPU0, Epoch{1}, InputStream, AllocationId{1}};
    auto bad = identity; bad.Allocation = AllocationId::Invalid;
    Reject([&] { identities.RegisterResource(a, bad, true); }, "Cross-adapter view without allocation accepted");
    Reject([&] { identities.RegisterResource(a, bad); }, "Transfer view hid its missing allocation");
    bad = identity; bad.Id = ResourceId::Invalid;
    Reject([&] { identities.RegisterResource(a, bad); }, "Invalid resource identity accepted");
    identities.RegisterResource(a, identity);
    Reject([&] { identities.RegisterResource(b, identity); }, "Duplicate device view identity accepted");
    Reject([&] { identities.ImportVersion(a, InputVersion); }, "Created view treated as external import");
    Reject([&] { identities.GetCompiledPlan(); }, "Uncompiled plan exposed");

    for (unsigned failure = 0; failure < 10; ++failure)
    {
        Graph graph;
        const auto resource = View(graph, "physical", GPU0, AllocationId{1});
        Placement location{ failure == 0 ? GPU1 : GPU0, (failure == 1 || failure == 7) ? QueueKind::Copy : QueueKind::Compute };
        auto state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        auto mode = Mode::Write;
        if (failure == 1 || failure == 2) state = D3D12_RESOURCE_STATE_RENDER_TARGET;
        if (failure == 7) { state = D3D12_RESOURCE_STATE_COPY_DEST; mode = Mode::ReadWrite; }
        if (failure == 8) state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        if (failure == 9) { state = D3D12_RESOURCE_STATE_COPY_DEST; mode = Mode::Read; }
        Pass pass("invalid contract", location, {{resource, mode, state}}, true);
        if (failure == 3) pass.Complete = false;
        if (failure == 4) pass.HiddenBuffer = true;
        if (failure == 5) pass.Supported = false;
        if (failure == 6) { location.Queue = static_cast<QueueKind>(255); }
        if (failure == 6)
        {
            Pass invalid("invalid placement", location, {}, true);
            Reject([&] { graph.AddPass(invalid); }, "Invalid queue placement accepted");
        }
        else
        {
            graph.AddPass(pass);
            Reject([&] { graph.CompilePlan(); }, "Invalid placement/access contract accepted");
        }
    }
    Graph unmanaged;
    const auto hidden = unmanaged.Import("unmanaged hidden resource");
    Pass tagged("placement is not memory registration", {GPU1, QueueKind::Compute},
        {{hidden, Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE}}, true);
    unmanaged.AddPass(tagged);
    Reject([&] { unmanaged.CompilePlan(); }, "Placement made unmanaged resource safe");

    for (unsigned failure = 0; failure < 5; ++failure)
    {
        Graph graph;
        const auto resource = View(graph, "versioned import", GPU0, AllocationId{1}, InputStream, true);
        if (failure != 0) graph.ImportVersion(resource, InputVersion);
        auto version = InputVersion;
        if (failure == 1) version.Invocation = ProducerInvocationId{8};
        if (failure == 2) version.Transfer = OutputStream;
        if (failure == 3) version.VersionEpoch = Epoch{2};
        Pass read("bad exact input", {GPU0, QueueKind::Direct},
            {{resource, Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, failure == 4 ? TransferVersion{} : version}}, true);
        graph.AddPass(read);
        Reject([&] { graph.CompilePlan(); }, "Missing/stale/wrong-stream/wrong-epoch version accepted");
    }
}

static void ExactFallbackAndAliasedWrites()
{
    for (bool directExternalRead : {false, true})
    {
        Graph graph;
        const auto external = View(graph, "exact external fallback", GPU0, AllocationId{1}, InputStream, true);
        const auto conditional = View(graph, "written conditional output", GPU0, AllocationId{2}, InputStream);
        graph.ImportVersion(external, InputVersion);
        graph.AddFallback(conditional, external);
        Pass writer("conditional writer", {GPU0, QueueKind::Direct}, {{conditional, Mode::Write, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, InputVersion}});
        std::vector<Graph::Access> reads{{conditional, Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, InputVersion}};
        if (directExternalRead) reads.push_back({external, Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, InputVersion});
        Pass consumer("conditional consumer", {GPU0, QueueKind::Direct}, std::move(reads), true);
        graph.AddPass(writer); const auto reader = graph.AddPass(consumer); graph.CompilePlan();
        const auto& inputs = graph.GetCompiledPlan().Inputs;
        Require(inputs.size() == 1 && inputs[0].View == external && inputs[0].Version == InputVersion &&
            inputs[0].Consumer == Graph::PlannedBatchId{reader}, directExternalRead ?
            "Direct external read doubled exact fallback readiness" : "Conditional-only read lost external exact fallback readiness");
    }

    for (bool differentStream : {false, true})
    {
        Graph aliases;
        const auto a = View(aliases, "alias A", GPU0, AllocationId{1}, InputStream);
        const auto b = View(aliases, "alias B", GPU0, AllocationId{1}, differentStream ? OutputStream : InputStream);
        auto other = differentStream ? OutputVersion : InputVersion;
        if (!differentStream) other.Invocation = ProducerInvocationId{8};
        Pass bad("incompatible writes", {GPU0, QueueKind::Compute}, {
            {a, Mode::Write, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, InputVersion},
            {b, Mode::Write, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, other}}, true);
        aliases.AddPass(bad);
        Reject([&] { aliases.CompilePlan(); }, "Conflicting versions of one allocation were silently overwritten in one pass");
    }
}

static void ForwardingAndLegacy()
{
    Graph graph;
    const auto input = View(graph, "input", GPU0, AllocationId{1}, TransferId::Invalid, true);
    const auto forwarded = View(graph, "forwarded", GPU0, AllocationId{2});
    const auto unused = View(graph, "unused", GPU1, AllocationId{3});
    graph.AddFallback(forwarded, input);
    Pass dead("dead", {GPU1, QueueKind::Compute}, {{unused, Mode::Write, D3D12_RESOURCE_STATE_UNORDERED_ACCESS}});
    Pass forward("forward", {GPU0, QueueKind::Direct}, {{forwarded, Mode::Write, D3D12_RESOURCE_STATE_RENDER_TARGET}});
    Pass consumer("consume", {GPU0, QueueKind::Direct}, {{forwarded, Mode::Read, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE}}, true);
    graph.AddPass(dead); const auto f = graph.AddPass(forward), c = graph.AddPass(consumer);
    graph.Export(forwarded); graph.CompilePlan();
    Require(graph.GetExecutionOrder() == std::vector<Graph::Pass>{f, c} && graph.GetStatistics().CulledPasses == 1 &&
        graph.GetCompiledPlan().Views[forwarded].Fallbacks == std::vector<Graph::Resource>{input}, "Culling/export/forwarding changed in placement plan");
    Graph legacy;
    const auto output = legacy.Import("output");
    TestPass direct("direct", {{output, Mode::Write}}), compute("compute", {{output, Mode::ReadWrite}});
    compute.Async = true;
    legacy.AddPass(direct); legacy.AddPass(compute); legacy.Compile();
    const auto& plan = legacy.GetCompiledPlan();
    Require(!plan.RequiresNewExecutor && plan.Batches.size() == 2 && plan.Batches[0].LegacyPrimary &&
        plan.Batches[1].LegacyPrimary && plan.Batches[0].Location.Queue == QueueKind::Direct &&
        plan.Batches[1].Location.Queue == QueueKind::Compute && Edge(plan, 0, 1), "Legacy DIRECT/async contract changed");
}

static void Run()
{
    HAO(); IndependentProducer(); ExternalProvenance(); AliasesAndFIFO(); InvalidContracts(); ExactFallbackAndAliasedWrites(); ForwardingAndLegacy();
    std::cout << "Placement compiler: HAO, independent invocations, exact versions, allocation aliases, FIFO cycles and legacy passed.\n";
}
}

class TestApp final : public App { public: TestApp() : App(GetModuleHandleW(nullptr)) {} };

class ForwardPass final : public GDX12RenderPass
{
public:
    ForwardPass(IRenderPassLink* input, IRenderPassLink* output) : Input(input), Output(output) {}
    bool FailRecording = false;
    void RecordRenderGraph(GDX12RenderGraphBuilder& graph) override
    {
        graph.SetName("Forward link");
        graph.Read(Input);
        graph.Read(Input); // The same input can occupy several shader bindings.
        graph.Write(Output, "Forwarded texture", Lifetime::Dynamic);
        graph.Fallback(Output, Input);
        graph.Export(Output);
        if (FailRecording) throw std::runtime_error("Failed pass setup");
    }
private:
    IRenderPassLink* Input;
    IRenderPassLink* Output;
};

static void TestPassRecording()
{
    Graph graph;
    GDX12Texture input, intermediate, output;
    ForwardPass producer(&input, &intermediate), consumer(&intermediate, &output);
    Reject([&] { graph.AddPass(producer); }, "Undeclared input link accepted");
    graph.ImportLink("External input", &input);
    Reject([&] { graph.ImportLink("Duplicate input", &input); }, "Duplicate imported link accepted");
    producer.FailRecording = true;
    Reject([&] { graph.AddPass(producer); }, "Recording exception lost");
    Reject([&] { graph.AddPass(consumer); }, "Failed recording leaked its output registration");
    producer.FailRecording = false;
    const auto first = graph.AddPass(producer), second = graph.AddPass(consumer);
    graph.Compile();
    Require(graph.GetExecutionOrder() == std::vector<Graph::Pass>{ first, second }, "Link dependencies or forwarding lost");
    GDX12RenderPass legacy;
    Reject([&] { graph.AddPass(legacy); }, "Pass without a graph contract accepted");
    std::cout << "Class recording, linked inputs, forwarding and failed-recording rollback passed.\n";
}

static void TestPooling(GDX12Device& device)
{
    GDX12DeviceResources resources;
    resources.Initialize(&device);
    GDX12Texture a, b;
    for (auto* texture : { &a, &b })
        GDX12RenderHelpers::InitializeRenderTarget(*texture, resources, 64, 32, DXGI_FORMAT_R8G8B8A8_UNORM);
    const auto srv = b.GetSRV()->HeapIndex, rtv = b.GetRTV()->HeapIndex;
    auto footprint = D3D12_PLACED_SUBRESOURCE_FOOTPRINT{};
    UINT64 bytes = 0;
    auto desc = a.GetResource()->D3DResource->GetDesc();
    device.GetDevice()->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    auto readbackA = device.CreateResource(D3D12_HEAP_TYPE_READBACK, CD3DX12_RESOURCE_DESC::Buffer(bytes), D3D12_RESOURCE_STATE_COPY_DEST);
    auto readbackB = device.CreateResource(D3D12_HEAP_TYPE_READBACK, CD3DX12_RESOURCE_DESC::Buffer(bytes), D3D12_RESOURCE_STATE_COPY_DEST);
    Graph graph(&device);
    const auto first = graph.Create("A", &a, true), second = graph.Create("B", &b, true);
    const auto resultA = graph.Import("readback A"), resultB = graph.Import("readback B");
    ClearPass clearA("clear A", first, a, { 1, 0, 0, 1 });
    CopyPass copyA("read A", first, resultA, a, readbackA.Get(), footprint);
    ClearPass clearB("clear B", second, b, { 0, 1, 0, 1 });
    CopyPass copyB("read B", second, resultB, b, readbackB.Get(), footprint);
    graph.AddPass(clearA);
    graph.AddPass(copyA);
    graph.AddPass(clearB);
    graph.AddPass(copyB);
    auto* queue = device.GetCommandQueue();
    for (bool reuse : { false, true, false, true })
    {
        graph.Compile(reuse);
        Require(graph.GetStatistics().PhysicalTextures == (reuse ? 1u : 2u), "Incorrect physical texture count");
        Require((a.GetResource() == b.GetResource()) == reuse, "Resource/state ownership was not shared or detached correctly");
        Require(b.GetSRV()->HeapIndex == srv && b.GetRTV()->HeapIndex == rtv, "Pooling changed descriptor indices");
        for (UINT frame = 0; frame < 4; ++frame)
        {
            auto* commands = queue->GetCommandList();
            graph.Execute(commands);
            queue->ExecuteCommandList(commands);
        }
        queue->Flush();
        for (UINT target = 0; target < 2; ++target)
        {
            auto& buffer = target ? readbackB : readbackA;
            BYTE* mapped = nullptr;
            ThrowIfFailed(buffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped)));
            for (UINT y = 0; y < 32; ++y)
                for (UINT x = 0; x < 64; ++x)
                {
                    auto* pixel = mapped + footprint.Offset + y * footprint.Footprint.RowPitch + x * 4;
                    Require(pixel[0] == (target ? 0 : 255) && pixel[1] == (target ? 255 : 0) && pixel[2] == 0 && pixel[3] == 255,
                        "Pooled output differs from dedicated output");
                }
            buffer->Unmap(0, nullptr);
        }
    }
    // A passthrough output keeps A live even after B is produced.
    const auto forwarded = graph.Create("forwarded A");
    graph.AddFallback(forwarded, first);
    TestPass forward("forward", {{ first, Mode::Read }, { forwarded, Mode::Write }});
    graph.AddPass(forward);
    graph.Export(forwarded);
    graph.Compile();
    Require(graph.GetStatistics().PhysicalTextures == 2, "Fallback/export lifetime allowed overlapping resources to share memory");
    queue->Flush();
    a.Resize(63, 31);
    Reject([&] { graph.Execute(nullptr); }, "Resize did not invalidate graph bindings");
    graph.Compile();
    Require(graph.GetStatistics().PhysicalTextures == 2, "Incompatible dimensions shared a resource");
    std::ostringstream dot;
    graph.WriteDot(dot);
    Require(dot.str().find("clear A") != std::string::npos, "Graph diagnostic export is empty");
    std::cout << "GPU pooled/dedicated identity, queued frames, shared states, descriptors, resize, exports and fallback lifetimes passed.\n";
}

class BufferCopyPass final : public GDX12RenderPass
{
public:
    BufferCopyPass(GDX12Resource& buffer, ID3D12Resource* transfer, bool write)
        : Async(write), Buffer(buffer), Transfer(transfer), Write(write) {}
    bool Async;
    UINT64 TransferOffset = 0;
    void RecordRenderGraph(GDX12RenderGraphBuilder& graph) override
    {
        graph.SetName(Write ? "Async buffer producer" : "Physical buffer reader");
        graph.HasSideEffects();
        if (Async) graph.AsyncCompute();
    }
    std::vector<Graph::BufferAccess> GetBufferAccesses() const override
    { return {{ &Buffer, Write ? Mode::Write : Mode::Read }}; }
    void Execute(GDX12CommandList* commands) override
    {
        commands->ResourceBarrier({ Write ? Buffer.GetCopyDestBarrier() : Buffer.GetCopySourceBarrier() });
        if (Write) commands->GetCommandList()->CopyBufferRegion(Buffer.D3DResource.Get(), 0, Transfer, TransferOffset, sizeof(UINT));
        else commands->GetCommandList()->CopyBufferRegion(Transfer, TransferOffset, Buffer.D3DResource.Get(), 0, sizeof(UINT));
    }
private:
    GDX12Resource& Buffer;
    ID3D12Resource* Transfer;
    bool Write;
};

static void TestAsyncTexture(GDX12Device& device)
{
    GDX12DeviceResources resources;
    resources.Initialize(&device);
    GDX12Texture texture;
    GDX12RenderHelpers::InitializeRenderTarget(texture, resources, 256, 256, DXGI_FORMAT_R8G8B8A8_UNORM);
    texture.GetResource()->D3DResource->SetName(L"Concurrent graph texture");
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT64 size;
    const auto desc = texture.GetResource()->D3DResource->GetDesc();
    device.GetDevice()->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &size);
    constexpr UINT frames = 400;
    auto readback = device.CreateResource(D3D12_HEAP_TYPE_READBACK, CD3DX12_RESOURCE_DESC::Buffer(size * frames), D3D12_RESOURCE_STATE_COPY_DEST);
    auto graphicsReadback = device.CreateResource(D3D12_HEAP_TYPE_READBACK, CD3DX12_RESOURCE_DESC::Buffer(size * frames), D3D12_RESOURCE_STATE_COPY_DEST);
    Graph graph(&device);
    const auto input = graph.Create("Shared async texture", &texture, true), output = graph.Import("Readback");
    ClearPass clear("Texture producer", input, texture, {1,0,0,1});
    CopyPass copy("Async texture reader", input, output, texture, readback.Get(), footprint);
    CopyPass graphicsCopy("Concurrent DIRECT texture reader", input, graph.Import("Graphics readback"), texture, graphicsReadback.Get(), footprint);
    copy.Async = true;
    graph.AddPass(clear); graph.AddPass(copy); graph.AddPass(graphicsCopy); graph.Compile();
    auto* queue = device.GetCommandQueue();
    for (UINT frame = 0; frame < frames; ++frame)
    {
        clear.SetRed(float(frame % 251) / 255);
        copy.SetOffset(frame * size);
        graphicsCopy.SetOffset(frame * size);
        auto* commands = graph.Execute(queue->GetCommandList(), device.TryGetComputeQueue());
        queue->ExecuteCommandList(commands);
        Require(texture.GetResource()->GetCurrentState() ==
            (D3D12_RESOURCE_STATE_COPY_SOURCE | D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE), "Shared texture union was not published");
    }
    queue->Flush();
    for (auto* buffer : { readback.Get(), graphicsReadback.Get() })
    {
        BYTE* pixels;
        ThrowIfFailed(buffer->Map(0, nullptr, reinterpret_cast<void**>(&pixels)));
        for (UINT frame = 0; frame < frames; ++frame)
            Require(pixels[frame * size] == frame % 251 && pixels[frame * size + 1] == 0,
                "Concurrent texture reader observed stale producer contents");
        buffer->Unmap(0, nullptr);
    }
    Require(graph.GetStatistics().JoinBefore == "frame retirement", "Shared texture reader joined its owner early");
    for (bool unmanaged : { false, true })
    {
        Graph guarded(&device);
        const auto shared = guarded.Import("Shared texture", &texture);
        TestPass asyncRead("Async guarded reader", {{ shared, Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE }}, true);
        asyncRead.Async = true;
        TestPass hazard("Texture hazard", {{ shared, unmanaged ? Mode::Read : Mode::Write,
            unmanaged ? Graph::UnmanagedState : D3D12_RESOURCE_STATE_RENDER_TARGET }}, true);
        guarded.AddPass(asyncRead); guarded.AddPass(hazard); guarded.Compile();
        queue->ExecuteCommandList(guarded.Execute(queue->GetCommandList(), device.TryGetComputeQueue()));
        queue->Flush();
        Require(guarded.GetStatistics().JoinBefore == "Texture hazard", "Writer or unmanaged state escaped the queue join");
    }
    std::cout << "Concurrent texture readers: 400 changing frames, both readbacks, writer/unmanaged joins passed.\n";
}

static void TestMultipleAsyncQueues(GDX12Device& device)
{
    auto* direct = device.GetCommandQueue();
    auto* compute = device.TryGetComputeQueue();
    Require(compute, "COMPUTE queue unavailable for multiple-producer fixture");
    constexpr UINT frames = 12;
    const auto desc = CD3DX12_RESOURCE_DESC::Buffer(256 * frames);
    GDX12Resource a, b;
    a.D3DResource = device.CreateResource(D3D12_HEAP_TYPE_DEFAULT, desc, D3D12_RESOURCE_STATE_COMMON);
    b.D3DResource = device.CreateResource(D3D12_HEAP_TYPE_DEFAULT, desc, D3D12_RESOURCE_STATE_COMMON);
    auto upload = device.CreateResource(D3D12_HEAP_TYPE_UPLOAD, desc, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto readback = device.CreateResource(D3D12_HEAP_TYPE_READBACK, desc, D3D12_RESOURCE_STATE_COPY_DEST);
    auto intermediate = device.CreateResource(D3D12_HEAP_TYPE_READBACK, desc, D3D12_RESOURCE_STATE_COPY_DEST);
    UINT* mapped;
    ThrowIfFailed(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)));
    for (UINT i = 0; i < frames; ++i) mapped[64 * i] = 0x12340000 + i;
    upload->Unmap(0, nullptr);
    const auto requireSamples = [&](ID3D12Resource* resource, const char* message)
    {
        ThrowIfFailed(resource->Map(0, nullptr, reinterpret_cast<void**>(&mapped)));
        for (UINT i = 0; i < frames; ++i) Require(mapped[64 * i] == 0x12340000 + i, message);
        resource->Unmap(0, nullptr);
    };
    BufferCopyPass first(a, upload.Get(), true), second(b, upload.Get(), true);
    BufferCopyPass consume(a, readback.Get(), false);
    TestPass graphics("Independent graphics", {}, true);
    for (bool async : {false, true, false})
        for (bool hasConsumer : {true, false})
        {
            Graph graph(&device);
            graph.AddPass(first); graph.AddPass(graphics); graph.AddPass(second);
            if (hasConsumer) graph.AddPass(consume);
            graph.Compile();
            UINT64 finalFence = 0;
            for (UINT frame = 0; frame < frames; ++frame)
            {
                first.TransferOffset = second.TransferOffset = consume.TransferOffset = frame * 256;
                auto* commands = graph.Execute(direct->GetCommandList(), async ? compute : nullptr);
                if (!hasConsumer) consume.Execute(commands);
                direct->ExecuteCommandList(commands);
                finalFence = commands->FenceValue;
            }
            direct->CPUWaitForFenceValue(finalFence);
            requireSamples(readback.Get(), "First pending producer or final fence lost queued data");
            Require(graph.GetStatistics().AsyncPasses == (async ? 2u : 0u), "Multiple async producers lost DIRECT fallback");
            Require(graph.GetStatistics().AsyncJoins == (async ? (hasConsumer ? 2u : 1u) : 0u), "Independent async producer lost prefix retirement");
            if (async)
                Require(graph.GetStatistics().JoinBefore == "frame retirement",
                    "First pending producer joined at the wrong consumer");
        }
    // COMPUTE RAW -> WAR -> WAW stays ordered on one queue without a DIRECT round trip.
    BufferCopyPass asyncRead(a, intermediate.Get(), false), overwrite(a, upload.Get(), true);
    asyncRead.Async = true;
    Graph ordered(&device);
    ordered.AddPass(first); ordered.AddPass(asyncRead); ordered.AddPass(overwrite); ordered.AddPass(consume);
    ordered.Compile();
    for (UINT frame = 0; frame < frames; ++frame)
    {
        first.TransferOffset = asyncRead.TransferOffset = consume.TransferOffset = frame * 256;
        overwrite.TransferOffset = ((frame + 1) % frames) * 256;
        direct->ExecuteCommandList(ordered.Execute(direct->GetCommandList(), compute));
    }
    direct->Flush();
    requireSamples(intermediate.Get(), "COMPUTE RAW/WAR ordering lost the intermediate value");
    ThrowIfFailed(readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped)));
    for (UINT frame = 0; frame < frames; ++frame)
        Require(mapped[64 * frame] == 0x12340000 + ((frame + 1) % frames), "COMPUTE WAW or DIRECT RAW ordering failed");
    readback->Unmap(0, nullptr);
    Require(ordered.GetStatistics().AsyncPasses == 3 && ordered.GetStatistics().AsyncJoins == 1,
        "Same-queue buffer dependencies introduced a premature DIRECT join");

    Graph logical(&device);
    first.TransferOffset = second.TransferOffset = 0;
    const auto oldest = logical.AddPass(first);
    logical.AddPass(second);
    const auto dependent = logical.AddPass(graphics);
    logical.DependsOn(dependent, oldest);
    logical.Compile();
    direct->ExecuteCommandList(logical.Execute(direct->GetCommandList(), compute)); direct->Flush();
    Require(logical.GetStatistics().AsyncJoins == 2 && logical.GetStatistics().JoinBefore == "frame retirement",
        "Logical dependency on the older pending producer was forgotten");

    // A DIRECT write must wait for an older COMPUTE reader or writer.
    overwrite.Async = false;
    for (bool reader : {true, false})
    {
        Graph hazard(&device);
        hazard.AddPass(reader ? asyncRead : first); hazard.AddPass(second); hazard.AddPass(overwrite); hazard.Compile();
        direct->ExecuteCommandList(hazard.Execute(direct->GetCommandList(), compute)); direct->Flush();
        Require(hazard.GetStatistics().AsyncJoins == 2 && hazard.GetStatistics().JoinBefore == "frame retirement",
            "DIRECT WAR/WAW against the older pending access escaped the join");
    }
    class RecordingFailure final : public TestPass
    {
    public:
        RecordingFailure() : TestPass("Failure after multiple submissions", {}, true) {}
        void Execute(GDX12CommandList*) override { throw std::runtime_error("Multiple-producer recording failure"); }
    } failure;
    Graph failed(&device);
    failed.AddPass(first); failed.AddPass(second); failed.AddPass(failure); failed.Compile();
    Reject([&] { failed.Execute(direct->GetCommandList(), compute); }, "Multiple-producer recording failure was swallowed");
    Require(failed.GetStatistics().AsyncPasses == 2 &&
        compute->GetFence()->GetCompletedValue() >= failed.GetStatistics().ComputeFence,
        "Recording failure left an older COMPUTE producer in flight");
    std::cout << "Multiple async producers: queued RAW/WAR/WAW, logical dependencies, retirement and DIRECT fallback passed.\n";
}

class UAVFillPass final : public TestPass
{
public:
    UAVFillPass(const char* name, Graph::Resource token, GDX12Texture& texture, GDX12DeviceResources& resources,
        float value, Mode usage = Mode::Write)
        : TestPass(name, {{ token, usage, D3D12_RESOURCE_STATE_UNORDERED_ACCESS }}, true),
          Texture(texture), Resources(resources), Value(value),
          CPUHeap(resources.Device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE) { Async = true; }
    void Execute(GDX12CommandList* commands) override
    {
        commands->SetDescriptorHeaps({ Resources.SRV_UAV_Heap.get() });
        const float values[] = { Value, Value, Value, Value };
        D3D12_UNORDERED_ACCESS_VIEW_DESC view = {}; view.Format = Texture.GetFormat(); view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        Resources.Device->GetDevice()->CreateUnorderedAccessView(Texture.GetResource()->D3DResource.Get(), nullptr, &view, CPUHeap.GetCPUHandle());
        commands->GetCommandList()->ClearUnorderedAccessViewFloat(Texture.GetUAV()->GPUHandle,
            CPUHeap.GetCPUHandle(), Texture.GetResource()->D3DResource.Get(), values, 0, nullptr);
    }
private:
    GDX12Texture& Texture;
    GDX12DeviceResources& Resources;
    float Value;
    GDX12DescriptorHeap CPUHeap;
};

static void TestAsyncTextureWrites(GDX12Device& device)
{
    auto* direct = device.GetCommandQueue(); auto* compute = device.TryGetComputeQueue();
    GDX12DeviceResources resources; resources.Initialize(&device);
    GDX12Texture a, b;
    GDX12RenderHelpers::InitializeComputeTexture(a, resources, 32, 16, DXGI_FORMAT_R32_FLOAT);
    GDX12RenderHelpers::InitializeComputeTexture(b, resources, 32, 16, DXGI_FORMAT_R32_FLOAT);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {}; UINT64 size;
    const auto desc = a.GetResource()->D3DResource->GetDesc();
    device.GetDevice()->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &size);
    auto readbackA = device.CreateResource(D3D12_HEAP_TYPE_READBACK, CD3DX12_RESOURCE_DESC::Buffer(size), D3D12_RESOURCE_STATE_COPY_DEST);
    auto readbackB = device.CreateResource(D3D12_HEAP_TYPE_READBACK, CD3DX12_RESOURCE_DESC::Buffer(size), D3D12_RESOURCE_STATE_COPY_DEST);
    const auto check = [&](ID3D12Resource* readback, float value)
    {
        BYTE* data; ThrowIfFailed(readback->Map(0, nullptr, reinterpret_cast<void**>(&data)));
        for (UINT y = 0; y < 16; ++y)
            for (UINT x = 0; x < 32; ++x)
                Require(reinterpret_cast<float*>(data + footprint.Offset + y * footprint.Footprint.RowPitch)[x] == value,
                    "Async UAV texture handoff returned stale contents");
        readback->Unmap(0, nullptr);
    };
    for (bool async : {false, true, false})
    {
        Graph graph(&device);
        const auto x = graph.Create("UAV A", &a, true), y = graph.Create("UAV B", &b, true);
        UAVFillPass first("UAV producer A", x, a, resources, 1), second("UAV producer B", y, b, resources, 2);
        CopyPass readA("UAV consumer A", x, graph.Import("Readback A"), a, readbackA.Get(), footprint);
        CopyPass readB("UAV consumer B", y, graph.Import("Readback B"), b, readbackB.Get(), footprint);
        // Interleaved consumers retire a prefix; whole-graph async lifetimes prevent aliasing.
        graph.AddPass(first); graph.AddPass(second); graph.AddPass(readA); graph.AddPass(readB); graph.Compile();
        Require(graph.GetStatistics().PhysicalTextures == 2, "Async UAV outputs shared a physical slot");
        UINT64 fence = 0;
        for (UINT frame = 0; frame < 12; ++frame)
        {
            auto* commands = graph.Execute(direct->GetCommandList(), async ? compute : nullptr);
            direct->ExecuteCommandList(commands); fence = commands->FenceValue;
        }
        direct->CPUWaitForFenceValue(fence); check(readbackA.Get(), 1); check(readbackB.Get(), 2);
        Require(graph.GetStatistics().AsyncJoins == (async ? 2u : 0u), "UAV consumer did not retire the needed prefix");
        std::ostringstream dot; graph.WriteDot(dot);
        if (async)
        {
            Require(dot.str().find("join before UAV consumer A") != std::string::npos &&
                dot.str().find("join before UAV consumer B") != std::string::npos, "Producer-specific UAV join diagnostics missing");
        }
        // Linear disjoint texture lifetimes still cannot alias across eligible async passes.
        Graph pooled(&device);
        const auto pa = pooled.Create("Async lifetime A", &a, true), pb = pooled.Create("Async lifetime B", &b, true);
        UAVFillPass fillA("Lifetime writer A", pa, a, resources, 1), fillB("Lifetime writer B", pb, b, resources, 2);
        CopyPass copyA("Lifetime reader A", pa, pooled.Import("Copy A"), a, readbackA.Get(), footprint);
        CopyPass copyB("Lifetime reader B", pb, pooled.Import("Copy B"), b, readbackB.Get(), footprint);
        pooled.AddPass(fillA); pooled.AddPass(copyA); pooled.AddPass(fillB); pooled.AddPass(copyB); pooled.Compile();
        Require(pooled.GetStatistics().PhysicalTextures == 2, "Linear pooling ignored async output lifetime");
    }
    {
        Graph graph(&device); const auto x = graph.Import("Retired output", &a), y = graph.Import("Retired second output", &b);
        UAVFillPass first("Retirement A", x, a, resources, 1), second("Retirement B", y, b, resources, 2);
        graph.AddPass(first); graph.AddPass(second); graph.Compile(false);
        auto* commands = graph.Execute(direct->GetCommandList(), compute);
        CopyPass read("After retirement", x, x, a, readbackA.Get(), footprint);
        read.Execute(commands); direct->ExecuteCommandList(commands); direct->Flush(); check(readbackA.Get(), 1);
        Require(graph.GetStatistics().AsyncJoins == 1 && graph.GetStatistics().JoinBefore == "frame retirement",
            "Unconsumed UAV output escaped final retirement");
    }
    // A later owner of a shared read layout survives retirement of an earlier owner.
    {
        Graph graph(&device); const auto token = graph.Import("Shared read texture", &a);
        CopyPass first("Shared reader one", token, graph.Import("First copy"), a, readbackA.Get(), footprint);
        CopyPass second("Shared reader two", token, graph.Import("Second copy"), a, readbackB.Get(), footprint);
        first.Async = second.Async = true;
        class Probe final : public TestPass
        {
        public:
            Probe(GDX12Resource& resource) : TestPass("Prefix lock probe", {}, true), Resource(resource) {}
            void Execute(GDX12CommandList*) override
            { Reject([&] { Resource.GetCommonBarrier(); }, "Prefix join unlocked a later texture reader"); }
        private: GDX12Resource& Resource;
        } probe(*a.GetResource());
        const auto older = graph.AddPass(first); graph.AddPass(second);
        const auto consumer = graph.AddPass(probe); graph.DependsOn(consumer, older); graph.Compile();
        direct->ExecuteCommandList(graph.Execute(direct->GetCommandList(), compute)); direct->Flush();
        Require(graph.GetStatistics().AsyncJoins == 2, "Shared readers were joined together before the first consumer");
        check(readbackA.Get(), 1); check(readbackB.Get(), 1);
        a.GetResource()->BeginConcurrentRead(); a.GetResource()->EndConcurrentRead();
    }
    GDX12Texture aliasTexture;
    GDX12RenderHelpers::InitializeComputeTexture(aliasTexture, resources, 32, 16, DXGI_FORMAT_R32_FLOAT);
    {
        Graph pool(&device);
        const auto x = pool.Create("Pool alias A", &a, true), y = pool.Create("Pool alias B", &aliasTexture, true);
        TestPass writeA("Pool write A", {{x, Mode::Write}}, true), readA("Pool read A", {{x, Mode::Read}}, true);
        TestPass writeB("Pool write B", {{y, Mode::Write}}, true), readB("Pool read B", {{y, Mode::Read}}, true);
        pool.AddPass(writeA); pool.AddPass(readA); pool.AddPass(writeB); pool.AddPass(readB); pool.Compile();
        Require(pool.GetStatistics().PhysicalTextures == 1 && a.GetResource() == aliasTexture.GetResource(),
            "Physical alias fixture did not share a tracked resource");
    }
    {
        GDX12Resource camera;
        camera.D3DResource = device.CreateResource(D3D12_HEAP_TYPE_DEFAULT, CD3DX12_RESOURCE_DESC::Buffer(256), D3D12_RESOURCE_STATE_COMMON);
        class CBVReader final : public TestPass
        {
        public:
            CBVReader(GDX12Resource& camera, bool async, bool probe = false)
                : TestPass(probe ? "Shared CBV probe" : "Shared CBV reader", {}, true), Camera(camera), Probe(probe) { Async = async; }
            std::vector<Graph::BufferAccess> GetBufferAccesses() const override
            { return {{ &Camera, Mode::Read, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER }}; }
            void Execute(GDX12CommandList*) override
            { if (Probe) Reject([&] { Camera.GetCommonBarrier(); }, "Prefix join unlocked a later CBV reader"); }
        private: GDX12Resource& Camera; bool Probe;
        } first(camera, true), second(camera, true), probe(camera, false, true);
        Graph graph(&device);
        const auto older = graph.AddPass(first); graph.AddPass(second);
        const auto consumer = graph.AddPass(probe); graph.DependsOn(consumer, older); graph.Compile();
        direct->ExecuteCommandList(graph.Execute(direct->GetCommandList(), compute)); direct->Flush();
        Require(graph.GetStatistics().AsyncJoins == 2 && camera.GetCurrentState() == D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
            "DEFAULT CBV readers lost their shared state or prefix retirement");
        camera.BeginConcurrentRead(); camera.EndConcurrentRead();
    }
    // Physical aliases, rather than matching logical IDs, carry WAW/RAW/WAR hazards.
    for (bool readerFirst : {false, true})
    {
        Graph graph(&device);
        const auto x = graph.Import("Alias writer", &a), alias = graph.Import("Alias reader/writer", &aliasTexture);
        UAVFillPass first("Alias UAV first", x, a, resources, 3);
        CopyPass priorRead("Alias async read", x, graph.Import("Prior readback"), a, readbackA.Get(), footprint);
        priorRead.Async = true;
        UAVFillPass second("Alias UAV second", alias, aliasTexture, resources, 4, Mode::ReadWrite);
        second.Async = !readerFirst;
        CopyPass finalRead("Alias DIRECT read", alias, graph.Import("Final readback"), aliasTexture, readbackB.Get(), footprint);
        if (readerFirst) graph.AddPass(priorRead); else graph.AddPass(first);
        graph.AddPass(second); graph.AddPass(finalRead); graph.Compile(false);
        direct->ExecuteCommandList(graph.Execute(direct->GetCommandList(), compute)); direct->Flush(); check(readbackB.Get(), 4);
        Require(graph.GetStatistics().AsyncJoins == 1, "Alias texture ordering introduced or missed a join");
    }
    {
        Graph graph(&device); const auto x = graph.Import("Bad async texture", &a);
        TestPass bad("Missing UAV state", {{x, Mode::Write}}, true); bad.Async = true;
        graph.AddPass(bad); graph.Compile();
        Reject([&] { graph.Execute(direct->GetCommandList(), compute); }, "Undeclared async UAV output accepted");
    }
    {
        class Failure final : public TestPass
        {
        public:
            Failure() : TestPass("Failure after UAV producers", {}, true) {}
            void Execute(GDX12CommandList*) override { throw std::runtime_error("UAV recording failure"); }
        } failure;
        Graph graph(&device); const auto x = graph.Import("Failure UAV A", &a), y = graph.Import("Failure UAV B", &b);
        UAVFillPass first("Failure writer A", x, a, resources, 5), second("Failure writer B", y, b, resources, 6);
        graph.AddPass(first); graph.AddPass(second); graph.AddPass(failure); graph.Compile();
        Reject([&] { graph.Execute(direct->GetCommandList(), compute); }, "UAV recording failure swallowed");
        Require(graph.GetStatistics().AsyncPasses == 2 && compute->GetFence()->GetCompletedValue() >= graph.GetStatistics().ComputeFence,
            "UAV recording failure left output owners in flight");
    }
    std::cout << "Async UAV textures: prefix consumers, retained read owners, RAW/WAR/WAW aliases, pooling, retirement/failure and fallback passed.\n";
}

static void TestAsyncQueues(GDX12Device& device)
{
    auto* direct = device.GetCommandQueue();
    auto* compute = device.TryGetComputeQueue();
    Require(compute, "COMPUTE queue unavailable for GPU fixture");
    const auto desc = CD3DX12_RESOURCE_DESC::Buffer(256);
    GDX12Resource buffer;
    buffer.D3DResource = device.CreateResource(D3D12_HEAP_TYPE_DEFAULT, desc, D3D12_RESOURCE_STATE_COMMON);
    auto upload = device.CreateResource(D3D12_HEAP_TYPE_UPLOAD, desc, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto readback = device.CreateResource(D3D12_HEAP_TYPE_READBACK, desc, D3D12_RESOURCE_STATE_COPY_DEST);
    UINT* mapped;
    ThrowIfFailed(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)));
    *mapped = 0x1234abcd;
    upload->Unmap(0, nullptr);
    BufferCopyPass producer(buffer, upload.Get(), true), consumer(buffer, readback.Get(), false);
    for (bool async : {false, true, false})
        for (bool hasConsumer : {true, false})
        {
            Graph graph(&device);
            graph.AddPass(producer);
            if (hasConsumer) graph.AddPass(consumer);
            graph.Compile();
            UINT64 finalFence = 0;
            for (UINT frame = 0; frame < 12; ++frame)
            {
                auto* commands = graph.Execute(direct->GetCommandList(), async ? compute : nullptr);
                if (!hasConsumer) consumer.Execute(commands);
                direct->ExecuteCommandList(commands);
                finalFence = commands->FenceValue;
            }
            direct->CPUWaitForFenceValue(finalFence);
            ThrowIfFailed(readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped)));
            Require(*mapped == 0x1234abcd, "Async buffer publication or final DIRECT retirement failed");
            readback->Unmap(0, nullptr);
            Require(graph.GetStatistics().AsyncPasses == (async ? 1u : 0u), "Async/direct scheduling differs from selection");
        }
    auto* wrongType = compute->GetCommandList();
    Reject([&] { direct->ExecuteCommandList(wrongType); }, "Wrong command-list queue type accepted");
    class RecordingFailure final : public TestPass
    {
    public:
        RecordingFailure() : TestPass("Recording failure", {}, true) {}
        void Execute(GDX12CommandList*) override { throw std::runtime_error("Fixture recording failure"); }
    } failure;
    Graph failed(&device);
    failed.AddPass(producer); failed.AddPass(failure); failed.Compile();
    Reject([&] { failed.Execute(direct->GetCommandList(), compute); }, "Recording failure was swallowed");
    Require(failed.GetStatistics().ComputeFence != 0 &&
        compute->GetFence()->GetCompletedValue() >= failed.GetStatistics().ComputeFence,
        "Failed recording left submitted simulation in flight");
    direct->Flush(); compute->Flush();
    TestMultipleAsyncQueues(device);
    TestAsyncTextureWrites(device);
    std::cout << "Async queues: physical RAW/WAR, DIRECT fallback, queued allocators, side-effect join and final fence passed.\n";
}

int main(int argc, char** argv)
{
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    try
    {
        TestCaptureClock();
        TestCompiler();
        TestPassRecording();
        PlanTests::Run();
        for (int i = 1; i < argc; ++i)
            if (std::string_view(argv[i]) == "--compiler-only")
            {
                std::cout << "Compiler-only tests passed; no GPU fixture initialized.\n";
                return 0;
            }
        TestApp app;
        ComPtr<ID3D12Debug> debug;
        ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
        debug->EnableDebugLayer();
        bool streamline = false;
        for (int i = 1; i < argc; ++i)
            if (std::string_view(argv[i]) == "--gpu-validation")
            {
                ComPtr<ID3D12Debug1> gbv;
                ThrowIfFailed(debug.As(&gbv));
                gbv->SetEnableGPUBasedValidation(TRUE);
            }
            else if (std::string_view(argv[i]) == "--streamline")
                streamline = true;
        if (streamline) Require(GDX12StreamlineSDK::Get().Initialize(), "Streamline initialization unavailable for fixture");
        GDX12Device device;
        ThrowIfFailed(device.Initialize(GDX12DeviceFactory::GetMostPerformantAdapter()));
        ComPtr<ID3D12InfoQueue> info;
        ThrowIfFailed(device.GetDevice().As(&info));
        info->ClearStoredMessages();
        TestPooling(device);
        TestAsyncQueues(device);
        TestAsyncTexture(device);
        for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i)
        {
            SIZE_T size = 0;
            ThrowIfFailed(info->GetMessage(i, nullptr, &size));
            std::vector<BYTE> storage(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            ThrowIfFailed(info->GetMessage(i, message, &size));
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) throw std::runtime_error(message->pDescription);
        }
        D3D12MA::TotalStatistics stats = {};
        device.GetMemoryAllocator()->CalculateStatistics(&stats);
        Require(stats.Total.Stats.AllocationCount == 0, "Graph leaked GPU allocations");
        std::cout << "Render graph tests passed; clean D3D12 debug layer.\n";
        return 0;
    }
    catch (const DxException& error) { std::wcerr << error.ToString() << '\n'; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
    return 1;
}

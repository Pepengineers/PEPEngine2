#include "App.Base/App.h"
#include "Engine.RendererDX12/GDX12RenderGraph.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include "Engine.RendererDX12/GDX12UploadBuffer.h"
#include "Engine.RendererDX12/GDX12StreamlineSDK.h"
#include <d3d12sdklayers.h>
#include <iostream>
#include <functional>
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

namespace PlacedTests
{
using namespace GDX12Submission;

constexpr UINT64 Bytes = 256;

class Pass final : public GDX12RenderPass
{
public:
    using Recorder = std::function<void(GDX12CommandList&, const Graph::BoundResources&)>;
    Pass(const char* name, Placement location, std::vector<Graph::Access> accesses, Recorder record)
        : Name(name), Location(location), Accesses(std::move(accesses)), Record(std::move(record)) {}
    void RecordRenderGraph(GDX12RenderGraphBuilder& builder) override
    {
        builder.SetName(Name);
        builder.SetPlacement(Location);
        builder.DeclareCompleteAccesses();
        builder.HasSideEffects();
        for (const auto& access : Accesses)
            if (access.Version.IsValid()) builder.UseVersion(access.Id, access.Usage, access.Version, access.State);
            else builder.Use(access.Id, access.Usage, access.State);
    }
    bool SupportsGraphQueue(QueueKind kind) const override { return Location.Queue == kind; }
    void RecordPlacedCommands(GDX12CommandList* list, const Graph::BoundResources& views) override
    { Record(*list, views); }
private:
    const char* Name;
    Placement Location;
    std::vector<Graph::Access> Accesses;
    Recorder Record;
};

static Pass Copy(const char* name, Placement location, Graph::Resource from, Graph::Resource to,
    TransferVersion input = {}, TransferVersion output = {})
{
    return Pass(name, location, {{from, Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE, input},
        {to, Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST, output}},
        [=](GDX12CommandList& list, const Graph::BoundResources& views) { list.CopyResource(views.Get(to), views.Get(from)); });
}

static std::shared_ptr<GDX12Resource> MakeResource(GDX12Device& device, D3D12_HEAP_TYPE type,
    const D3D12_RESOURCE_DESC& desc = CD3DX12_RESOURCE_DESC::Buffer(Bytes))
{
    auto view = std::make_shared<GDX12Resource>();
    const auto entry = type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ :
        type == D3D12_HEAP_TYPE_READBACK ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_COMMON;
    const CD3DX12_HEAP_PROPERTIES heap(type);
    ComPtr<ID3D12Resource> resource;
    ThrowIfFailed(device.GetDevice()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
        &desc, entry, nullptr, IID_PPV_ARGS(&resource)));
    view->D3DResource = GDX12ResourceHandle(std::move(resource));
    view->SetCurrentState(entry);
    return view;
}

static Graph::Resource Register(Graph& graph, Graph::ExecutionBindings& bindings, const char* name,
    GDX12Device& device, std::shared_ptr<GDX12Resource> view, uint64_t id, uint64_t allocation,
    TransferVersion version = {}, bool imported = false, bool shared = false)
{
    const auto key = imported ? graph.Import(name) : graph.Create(name);
    const ResourceIdentity identity{ ResourceId{id}, device.GetSubmissionAdapterId(),
        version.IsValid() ? version.VersionEpoch : Epoch{1}, version.Transfer, AllocationId{allocation} };
    graph.RegisterResource(key, identity, shared);
    bindings.Views.push_back({ key, identity, std::move(view) });
    return key;
}

static void Fill(GDX12Resource& resource, uint32_t generation)
{
    void* mapped = nullptr;
    const D3D12_RANGE empty{0, 0}, written{0, Bytes};
    ThrowIfFailed(resource.D3DResource->Map(0, &empty, &mapped));
    for (size_t i = 0; i < Bytes / sizeof(uint32_t); ++i)
        static_cast<uint32_t*>(mapped)[i] = generation * 7919u + static_cast<uint32_t>(i) * 31u;
    resource.D3DResource->Unmap(0, &written);
}

static void CheckPayload(GDX12Resource& resource, uint32_t generation)
{
    void* mapped = nullptr;
    const D3D12_RANGE read{0, Bytes}, empty{0, 0};
    ThrowIfFailed(resource.D3DResource->Map(0, &read, &mapped));
    bool equal = true;
    for (size_t i = 0; i < Bytes / sizeof(uint32_t); ++i)
        equal &= static_cast<const uint32_t*>(mapped)[i] == generation * 7919u + static_cast<uint32_t>(i) * 31u;
    resource.D3DResource->Unmap(0, &empty);
    Require(equal, "Placed graph payload generation/checksum corrupted");
}

// Test control only. A CPU gate signal never stands in for a queue-issued completion.
class Gate
{
public:
    Gate(GDX12Device& device, GDX12CommandQueue& queue, ComPtr<ID3D12Fence>& fence) : Fence(fence)
    {
        ThrowIfFailed(device.GetDevice()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&Fence)));
        ThrowIfFailed(queue.GetCommandQueue()->Wait(Fence.Get(), 1));
    }
    ~Gate() { if (!Opened) Fence->Signal(1); }
    void Open() { ThrowIfFailed(Fence->Signal(1)); Opened = true; }
private:
    ComPtr<ID3D12Fence>& Fence;
    bool Opened = false;
};

static void RequireReached(CompletionPoint point, GDX12CommandQueue& queue)
{
    const auto deadline = GetTickCount64() + 5000;
    while (!point.IsReachedBy(queue.GetCompletionProgress()) && GetTickCount64() < deadline) Sleep(1);
    Require(point.IsReachedBy(queue.GetCompletionProgress()), "Expected queue progress did not occur while another queue was gated");
}

static void LocalChain(GDX12Device& device)
{
    auto* direct = device.GetCommandQueue();
    auto* compute = device.TryGetComputeQueue();
    auto* copy = device.TryGetCopyQueue();
    Require(direct && compute && copy, "Placed fixture requires DIRECT, COMPUTE and COPY");
    const auto adapter = device.GetSubmissionAdapterId();
    auto upload = MakeResource(device, D3D12_HEAP_TYPE_UPLOAD);
    auto readback = MakeResource(device, D3D12_HEAP_TYPE_READBACK);
    Fill(*upload, 19);
    Graph graph;
    Graph::ExecutionBindings bindings;
    bindings.Queues = {{&device, direct}, {&device, compute}, {&device, copy}};
    const auto in = Register(graph, bindings, "upload", device, upload, 1, 1, {}, true);
    const auto a = Register(graph, bindings, "direct result", device, MakeResource(device, D3D12_HEAP_TYPE_DEFAULT), 2, 2);
    const auto b = Register(graph, bindings, "compute result", device, MakeResource(device, D3D12_HEAP_TYPE_DEFAULT), 3, 3);
    const auto c = Register(graph, bindings, "copy result", device, MakeResource(device, D3D12_HEAP_TYPE_DEFAULT), 4, 4);
    const auto out = Register(graph, bindings, "readback", device, readback, 5, 5, {}, true);
    auto load = Copy("DIRECT upload", {adapter, QueueKind::Direct}, in, a);
    auto computeCopy = Copy("COMPUTE copy", {adapter, QueueKind::Compute}, a, b);
    auto copyPass = Copy("COPY transfer", {adapter, QueueKind::Copy}, b, c);
    auto download = Copy("DIRECT readback", {adapter, QueueKind::Direct}, c, out);
    graph.AddPass(load); graph.AddPass(computeCopy); graph.AddPass(copyPass); graph.AddPass(download);
    graph.CompilePlan();

    Graph::PlanExecutor executor(1);
    const auto directBefore = direct->FenceValue, computeBefore = compute->FenceValue, copyBefore = copy->FenceValue;
    auto invalid = bindings;
    invalid.Queues.pop_back();
    Reject([&] { executor.TrySubmit(graph, invalid); }, "Missing COPY queue accepted");
    invalid = bindings;
    invalid.Views[b].Identity.ResourceEpoch = Epoch{2};
    Reject([&] { executor.TrySubmit(graph, invalid); }, "Wrong bound epoch accepted");
    invalid = bindings;
    invalid.Views[b].Native = invalid.Views[a].Native;
    Reject([&] { executor.TrySubmit(graph, invalid); }, "One native object registered under two resource IDs");
    Require(executor.PendingCount() == 0 && direct->FenceValue == directBefore && compute->FenceValue == computeBefore &&
        copy->FenceValue == copyBefore, "Invalid plan reached submission");
    const auto issued = executor.TrySubmit(graph, bindings);
    Require(issued && issued->Batches.size() == 4 && issued->Batches[0].Timeline() == direct->GetTimelineIdentity() &&
        issued->Batches[1].Timeline() == compute->GetTimelineIdentity() &&
        issued->Batches[2].Timeline() == copy->GetTimelineIdentity() &&
        issued->Batches[3].Timeline() == direct->GetTimelineIdentity(), "Wrong timelines in placed chain receipt");
    executor.Drain();
    CheckPayload(*readback, 19);
    std::cout << "Native placed DIRECT -> COMPUTE -> COPY -> DIRECT payload passed.\n";
}

static void ExactAndRetirement(GDX12Device& device)
{
    auto* direct = device.GetCommandQueue();
    auto* compute = device.TryGetComputeQueue();
    auto* copy = device.TryGetCopyQueue();
    const auto adapter = device.GetSubmissionAdapterId();
    auto upload = MakeResource(device, D3D12_HEAP_TYPE_UPLOAD);
    auto readback = MakeResource(device, D3D12_HEAP_TYPE_READBACK);
    auto local = MakeResource(device, D3D12_HEAP_TYPE_DEFAULT);
    Fill(*upload, 31);
    ComPtr<ID3D12Fence> importGateFence;
    Graph::PlanExecutor executor(1);
    Gate blockImport(device, *copy, importGateFence);
    auto* import = copy->GetCommandList();
    import->CopyResource(local->D3DResource.Get(), upload->D3DResource.Get());
    copy->ExecuteCommandList(import);
    const auto ready = copy->GetLastCompletionPoint();
    Require(ready.IsValid() && !ready.IsReachedBy(copy->GetCompletionProgress()), "Exact fixture did not issue a pending import");

    Graph graph;
    Graph::ExecutionBindings bindings;
    bindings.Queues = {{&device, direct}, {&device, compute}, {&device, copy}};
    const TransferVersion version{TransferId{41}, Epoch{1}, ProducerInvocationId{31}};
    const auto input = Register(graph, bindings, "reserved local snapshot", device, local, 11, 11, version, true);
    graph.ImportVersion(input, version);
    const auto out = Register(graph, bindings, "exact readback", device, readback, 12, 12, {}, true);
    auto owner = std::make_shared<int>(31);
    auto invocationOwner = std::make_shared<int>(97);
    const std::weak_ptr<int> weakOwner = owner, weakInvocation = invocationOwner;
    const std::weak_ptr<GDX12Resource> weakView = local;
    bindings.ExactInputs.push_back({input, version, ready, owner});
    bindings.KeepAlive.push_back(invocationOwner);

    const auto noop = [](GDX12CommandList&, const Graph::BoundResources&) {};
    Pass prefix("DIRECT independent prefix", {adapter, QueueKind::Direct}, {}, noop);
    auto consumer = Copy("COMPUTE exact reader", {adapter, QueueKind::Compute}, input, out, version);
    Pass frame("DIRECT independent frame tail", {adapter, QueueKind::Direct}, {}, noop);
    bool failRecording = true;
    Pass join("DIRECT dependent consumer", {adapter, QueueKind::Direct}, {},
        [&](GDX12CommandList&, const Graph::BoundResources&)
        {
            if (!failRecording) return;
            local->SetCurrentState(D3D12_RESOURCE_STATE_COPY_DEST);
            throw std::runtime_error("late recording failure");
        });
    graph.AddPass(prefix);
    const auto reader = graph.AddPass(consumer);
    graph.AddPass(frame);
    const auto joined = graph.AddPass(join);
    graph.DependsOn(joined, reader);
    graph.CompilePlan();

    const auto directBefore = direct->FenceValue, computeBefore = compute->FenceValue;
    auto invalid = bindings;
    invalid.ExactInputs.clear();
    Reject([&] { executor.TrySubmit(graph, invalid); }, "Missing exact input accepted");
    invalid = bindings;
    invalid.ExactInputs[0].Version.Invocation = ProducerInvocationId{30};
    Reject([&] { executor.TrySubmit(graph, invalid); }, "Stale exact input accepted");
    invalid = bindings;
    invalid.ExactInputs[0].Reservation.reset();
    Reject([&] { executor.TrySubmit(graph, invalid); }, "Exact input accepted without reservation lifetime");
    GDX12CommandQueue stale(&device, D3D12_COMMAND_LIST_TYPE_COPY);
    const auto stalePoint = stale.SignalCompletion();
    stale.Reset();
    invalid = bindings;
    invalid.ExactInputs[0].Ready = stalePoint;
    Reject([&] { executor.TrySubmit(graph, invalid); }, "Stale readiness timeline accepted");
    invalid = {};
    Reject([&] { executor.TrySubmit(graph, bindings); }, "Late recorder exception lost");
    Require(executor.PendingCount() == 0 && direct->FenceValue == directBefore && compute->FenceValue == computeBefore &&
        local->GetCurrentState() == D3D12_RESOURCE_STATE_COMMON, "Late recording failure submitted a prefix or leaked host resource state");
    failRecording = false;

    const auto issued = executor.TrySubmit(graph, bindings);
    Require(issued && issued->Batches.size() == 4, "Pending exact import prevented GPU-side submission");
    RequireReached(issued->Batches[2], *direct);
    Require(!issued->Batches[1].IsReachedBy(compute->GetCompletionProgress()) &&
        !issued->Batches[3].IsReachedBy(direct->GetCompletionProgress()), "Exact read or dependent DIRECT pass bypassed the gate");
    Require(!executor.TrySubmit(graph, bindings), "In-flight limit did not defer a blocked invocation");
    bindings = {}; owner.reset(); invocationOwner.reset(); local.reset();
    executor.CollectCompleted();
    Require(executor.PendingCount() == 1 && !weakOwner.expired() && !weakInvocation.expired() && !weakView.expired(),
        "Independent DIRECT completion released in-flight resource/slot owners");
    blockImport.Open();
    executor.Drain();
    Require(executor.PendingCount() == 0 && weakOwner.expired() && weakInvocation.expired() && weakView.expired(),
        "All timelines completed without retiring invocation owners");
    CheckPayload(*readback, 31);
    std::cout << "Issued pending Exact, pre-submit rollback, partial join, independent frame progress and retirement passed.\n";
}

static void TextureUAV(GDX12Device& device)
{
    auto* compute = device.TryGetComputeQueue();
    auto* copy = device.TryGetCopyQueue();
    const auto adapter = device.GetSubmissionAdapterId();
    const auto textureDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32_UINT, 2, 2, 1, 1, 1, 0,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto texture = MakeResource(device, D3D12_HEAP_TYPE_DEFAULT, textureDesc);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 required = 0;
    device.GetDevice()->GetCopyableFootprints(&textureDesc, 0, 1, 0, &footprint, nullptr, nullptr, &required);
    auto readback = MakeResource(device, D3D12_HEAP_TYPE_READBACK, CD3DX12_RESOURCE_DESC::Buffer(required));

    struct Heaps { ComPtr<ID3D12DescriptorHeap> Cpu, Gpu; };
    auto descriptors = std::make_shared<Heaps>();
    const std::weak_ptr<Heaps> weakDescriptors = descriptors;
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.NumDescriptors = 1;
    ThrowIfFailed(device.GetDevice()->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&descriptors->Cpu)));
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(device.GetDevice()->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&descriptors->Gpu)));
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R32_UINT;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    const auto cpu = descriptors->Cpu->GetCPUDescriptorHandleForHeapStart();
    auto* heap = descriptors->Gpu.Get();
    const auto gpu = heap->GetGPUDescriptorHandleForHeapStart();
    device.GetDevice()->CreateUnorderedAccessView(texture->D3DResource.Get(), nullptr, &uav, cpu);
    device.GetDevice()->CreateUnorderedAccessView(texture->D3DResource.Get(), nullptr, &uav, heap->GetCPUDescriptorHandleForHeapStart());

    Graph graph;
    Graph::ExecutionBindings bindings;
    bindings.Queues = {{&device, compute}, {&device, copy}};
    bindings.KeepAlive.push_back(descriptors);
    const auto target = Register(graph, bindings, "UAV texture", device, texture, 1, 1);
    const auto output = Register(graph, bindings, "readback footprint", device, readback, 2, 2, {}, true);
    const auto clear = [&](UINT value)
    {
        return Pass("COMPUTE UAV clear", {adapter, QueueKind::Compute},
            {{target, Mode::Write, D3D12_RESOURCE_STATE_UNORDERED_ACCESS}},
            [=](GDX12CommandList& list, const Graph::BoundResources& views)
            {
                auto* native = list.GetCommandList().Get();
                ID3D12DescriptorHeap* heaps[]{heap};
                const UINT pattern[]{value, 0, 0, 0};
                native->SetDescriptorHeaps(1, heaps);
                native->ClearUnorderedAccessViewUint(gpu, cpu, views.Get(target), pattern, 0, nullptr);
            });
    };
    auto write = clear(17), overwrite = clear(83);
    Pass download("COPY texture footprint", {adapter, QueueKind::Copy},
        {{target, Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE}, {output, Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST}},
        [=](GDX12CommandList& list, const Graph::BoundResources& views)
        {
            const CD3DX12_TEXTURE_COPY_LOCATION source(views.Get(target), 0);
            const CD3DX12_TEXTURE_COPY_LOCATION destination(views.Get(output), footprint);
            list.GetCommandList()->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        });
    graph.AddPass(write); graph.AddPass(overwrite); graph.AddPass(download); graph.CompilePlan();
    ComPtr<ID3D12Fence> gateFence;
    Graph::PlanExecutor executor(1);
    Gate blockUAV(device, *compute, gateFence);
    auto invalid = bindings;
    invalid.Views[target].Native = MakeResource(device, D3D12_HEAP_TYPE_DEFAULT,
        CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32_UINT, 2, 2, 1, 1));
    const auto before = compute->FenceValue;
    Reject([&] { executor.TrySubmit(graph, invalid); }, "Texture without UAV flag accepted");
    Require(compute->FenceValue == before, "Illegal UAV access submitted a batch");
    invalid = {};
    Require(executor.TrySubmit(graph, bindings).has_value(), "UAV texture submission unexpectedly deferred");
    bindings = {}; descriptors.reset();
    executor.CollectCompleted();
    Require(!weakDescriptors.expired(), "Pending GPU work lost shader-visible descriptor owner");
    blockUAV.Open();
    executor.Drain();
    Require(weakDescriptors.expired(), "Completed GPU work retained descriptor owner");
    void* mapped = nullptr;
    const D3D12_RANGE read{0, static_cast<SIZE_T>(required)}, empty{0, 0};
    ThrowIfFailed(readback->D3DResource->Map(0, &read, &mapped));
    bool equal = true;
    for (size_t row = 0; row < 2; ++row)
    {
        const auto* pixels = reinterpret_cast<const UINT*>(static_cast<const BYTE*>(mapped) + footprint.Offset + row * footprint.Footprint.RowPitch);
        equal &= pixels[0] == 83 && pixels[1] == 83;
    }
    readback->D3DResource->Unmap(0, &empty);
    Require(equal, "UAV texture overwrite / COMPUTE-to-COPY handoff corrupted readback");
    std::cout << "Native UAV texture WAW, COPY footprint and descriptor retirement passed.\n";
}

struct SharedBuffer
{
    ComPtr<ID3D12Heap> Heaps[2];
    GDX12Resource Views[2];
};

static std::shared_ptr<SharedBuffer> MakeSharedBuffer(GDX12Device& first, GDX12Device& second)
{
    const auto desc = CD3DX12_RESOURCE_DESC::Buffer(Bytes, D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER);
    const auto a = first.GetDevice()->GetResourceAllocationInfo(0, 1, &desc);
    const auto b = second.GetDevice()->GetResourceAllocationInfo(0, 1, &desc);
    const auto invalid = (std::numeric_limits<UINT64>::max)();
    Require(a.SizeInBytes != invalid && b.SizeInBytes != invalid, "Cross-adapter allocation unsupported");
    D3D12_HEAP_DESC heap{};
    heap.Alignment = (std::max)(a.Alignment, b.Alignment);
    heap.SizeInBytes = (std::max)(a.SizeInBytes, b.SizeInBytes);
    heap.Properties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    heap.Flags = D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER;
    auto storage = std::make_shared<SharedBuffer>();
    ThrowIfFailed(first.GetDevice()->CreateHeap(&heap, IID_PPV_ARGS(&storage->Heaps[0])));
    struct Handle { HANDLE Value = nullptr; ~Handle() { if (Value) CloseHandle(Value); } } handle;
    ThrowIfFailed(first.GetDevice()->CreateSharedHandle(storage->Heaps[0].Get(), nullptr, GENERIC_ALL, nullptr, &handle.Value));
    ThrowIfFailed(second.GetDevice()->OpenSharedHandle(handle.Value, IID_PPV_ARGS(&storage->Heaps[1])));
    GDX12Device* devices[]{&first, &second};
    for (size_t i = 0; i < 2; ++i)
    {
        ComPtr<ID3D12Resource> view;
        ThrowIfFailed(devices[i]->GetDevice()->CreatePlacedResource(storage->Heaps[i].Get(), 0, &desc,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&view)));
        storage->Views[i].D3DResource = GDX12ResourceHandle(std::move(view));
    }
    return storage;
}

static void RemoteExactAndReuse(GDX12Device& first, GDX12Device& second,
    GDX12CommandQueue& firstCopy, GDX12CommandQueue& secondCopy)
{
    auto storage = MakeSharedBuffer(first, second);
    auto readback1 = MakeResource(second, D3D12_HEAP_TYPE_READBACK);
    auto readback2 = MakeResource(second, D3D12_HEAP_TYPE_READBACK);
    ComPtr<ID3D12Fence> producerGateFence, readerGateFence;
    Graph::PlanExecutor executor(4);
    Gate holdProducer(first, firstCopy, producerGateFence), holdReader(second, secondCopy, readerGateFence);
    struct Publication { TransferVersion Version; CompletionPoint Ready; ResourceIdentity Source; };

    const auto publish = [&](unsigned generation)
    {
        const TransferVersion version{TransferId{71}, Epoch{1}, ProducerInvocationId{generation}};
        auto upload = MakeResource(first, D3D12_HEAP_TYPE_UPLOAD);
        Fill(*upload, generation);
        Graph graph;
        Graph::ExecutionBindings bindings;
        bindings.Queues = {{&first, &firstCopy}};
        const auto in = Register(graph, bindings, "upload", first, upload, 10 + generation, 10 + generation, {}, true);
        const auto out = Register(graph, bindings, "export alias", first, {storage, &storage->Views[0]}, 1, 1, version, false, true);
        auto copy = Copy("remote export", {first.GetSubmissionAdapterId(), QueueKind::Copy}, in, out, {}, version);
        graph.AddPass(copy); graph.CompilePlan();
        const auto receipt = executor.TrySubmit(graph, bindings);
        Require(receipt.has_value(), "Remote producer unexpectedly deferred");
        return Publication{version, receipt->Batches.front(), bindings.Views[out].Identity};
    };
    const auto read = [&](const Publication& publication, std::shared_ptr<GDX12Resource> output, uint64_t key, bool invalidProvenance)
    {
        Graph graph;
        Graph::ExecutionBindings bindings;
        bindings.Queues = {{&second, &secondCopy}};
        const auto in = Register(graph, bindings, "exact imported shared view", second, {storage, &storage->Views[1]},
            2, 1, publication.Version, true, true);
        graph.ImportVersion(in, publication.Version);
        const auto out = Register(graph, bindings, "readback", second, output, key, key, {}, true);
        auto source = publication.Source;
        if (invalidProvenance) source.Allocation = AllocationId{999};
        bindings.ExactInputs.push_back({in, publication.Version, publication.Ready, storage, &firstCopy, source});
        auto copy = Copy("remote exact read", {second.GetSubmissionAdapterId(), QueueKind::Copy}, in, out, publication.Version);
        graph.AddPass(copy); graph.CompilePlan();
        const auto receipt = executor.TrySubmit(graph, bindings);
        Require(receipt.has_value(), "Remote exact reader unexpectedly deferred");
        return receipt->Batches.front();
    };

    const auto firstReady = publish(101);
    Require(!firstReady.Ready.IsReachedBy(firstCopy.GetCompletionProgress()), "Remote ready fixture was not pending");
    const auto before = secondCopy.FenceValue;
    Reject([&] { read(firstReady, readback1, 301, true); }, "Remote export without matching allocation provenance accepted");
    Require(secondCopy.FenceValue == before, "Invalid remote exact binding submitted");
    const auto firstRead = read(firstReady, readback1, 301, false);
    const auto secondReady = publish(102);
    holdProducer.Open();
    RequireReached(firstReady.Ready, firstCopy);
    Require(!secondReady.Ready.IsReachedBy(firstCopy.GetCompletionProgress()) &&
        !firstRead.IsReachedBy(secondCopy.GetCompletionProgress()), "Reused allocation overwrote a pending remote reader");
    Reject([&] { read(firstReady, readback1, 301, false); }, "Old exact generation accepted after a newer alias writer was queued");
    const auto secondRead = read(secondReady, readback2, 302, false);
    Require(executor.PendingCount() >= 3 && !secondRead.IsReachedBy(secondCopy.GetCompletionProgress()),
        "Remote reader/overwrite chain bypassed its gate");
    holdReader.Open();
    executor.Drain();
    CheckPayload(*readback1, 101); CheckPayload(*readback2, 102);
    std::cout << "Remote issued Exact, shared provenance and cross-invocation alias WAR passed.\n";
}

static void TwoAdapters(GDX12Device& first, GDX12Device& second)
{
    const auto firstLuid = first.GetAdapterLuid(), secondLuid = second.GetAdapterLuid();
    Require(firstLuid.HighPart != secondLuid.HighPart || firstLuid.LowPart != secondLuid.LowPart, "Fixture used the same physical GPU twice");
    GDX12CommandQueue firstCopy(&first, D3D12_COMMAND_LIST_TYPE_COPY, true);
    GDX12CommandQueue secondCopy(&second, D3D12_COMMAND_LIST_TYPE_COPY, true);
    auto* compute = second.TryGetComputeQueue();
    Require(compute, "Secondary COMPUTE queue unavailable");
    const auto a = first.GetSubmissionAdapterId(), b = second.GetSubmissionAdapterId();
    auto inputShared = MakeSharedBuffer(first, second), outputShared = MakeSharedBuffer(first, second);
    auto source = MakeResource(first, D3D12_HEAP_TYPE_DEFAULT);
    auto imported = MakeResource(second, D3D12_HEAP_TYPE_DEFAULT);
    auto processed = MakeResource(second, D3D12_HEAP_TYPE_DEFAULT);
    auto returned = MakeResource(first, D3D12_HEAP_TYPE_DEFAULT);
    std::array<std::shared_ptr<GDX12Resource>, 2> readbacks;
    ComPtr<ID3D12Fence> secondGateFence;
    Graph::PlanExecutor executor(2);
    Gate holdSecondCopy(second, secondCopy, secondGateFence);

    for (unsigned generation = 0; generation < readbacks.size(); ++generation)
    {
        const TransferVersion vin{TransferId{51}, Epoch{1}, ProducerInvocationId{generation + 1u}};
        const TransferVersion vout{TransferId{52}, Epoch{1}, ProducerInvocationId{generation + 1u}};
        auto upload = MakeResource(first, D3D12_HEAP_TYPE_UPLOAD);
        Fill(*upload, generation + 73);
        readbacks[generation] = MakeResource(first, D3D12_HEAP_TYPE_READBACK);
        Graph graph;
        Graph::ExecutionBindings bindings;
        bindings.Queues = {{&first, first.GetCommandQueue()}, {&first, &firstCopy}, {&second, &secondCopy}, {&second, compute}};
        const auto in = Register(graph, bindings, "upload", first, upload, 100 + generation, 100 + generation, {}, true);
        const auto local0 = Register(graph, bindings, "source", first, source, 1, 1, vin);
        const auto shared0 = Register(graph, bindings, "export view", first, {inputShared, &inputShared->Views[0]}, 2, 2, vin, false, true);
        const auto shared1 = Register(graph, bindings, "import view", second, {inputShared, &inputShared->Views[1]}, 3, 2, vin, false, true);
        const auto local1 = Register(graph, bindings, "imported local", second, imported, 4, 4, vin);
        const auto result1 = Register(graph, bindings, "processed local", second, processed, 5, 5, vout);
        const auto resultShared1 = Register(graph, bindings, "result export view", second, {outputShared, &outputShared->Views[1]}, 6, 6, vout, false, true);
        const auto resultShared0 = Register(graph, bindings, "result import view", first, {outputShared, &outputShared->Views[0]}, 7, 6, vout, false, true);
        const auto result0 = Register(graph, bindings, "returned local", first, returned, 8, 8, vout);
        const auto out = Register(graph, bindings, "readback", first, readbacks[generation], 110 + generation, 110 + generation, {}, true);
        auto load = Copy("DIRECT0 source", {a, QueueKind::Direct}, in, local0, {}, vin);
        auto exportInput = Copy("COPY0 export", {a, QueueKind::Copy}, local0, shared0, vin, vin);
        auto importInput = Copy("COPY1 import", {b, QueueKind::Copy}, shared1, local1, vin, vin);
        auto solve = Copy("COMPUTE1 process", {b, QueueKind::Compute}, local1, result1, vin, vout);
        auto exportResult = Copy("COPY1 export", {b, QueueKind::Copy}, result1, resultShared1, vout, vout);
        auto importResult = Copy("COPY0 import", {a, QueueKind::Copy}, resultShared0, result0, vout, vout);
        auto read = Copy("DIRECT0 readback", {a, QueueKind::Direct}, result0, out, vout);
        graph.AddPass(load); graph.AddPass(exportInput); graph.AddPass(importInput); graph.AddPass(solve);
        graph.AddPass(exportResult); graph.AddPass(importResult); graph.AddPass(read);
        graph.CompilePlan();
        if (!generation)
        {
            auto invalid = bindings;
            invalid.Queues[1].Queue = first.TryGetCopyQueue();
            Require(invalid.Queues[1].Queue, "Local COPY queue unavailable for capability negative case");
            const auto firstBefore = first.GetCommandQueue()->FenceValue, secondBefore = secondCopy.FenceValue;
            Reject([&] { executor.TrySubmit(graph, invalid); }, "Cross-adapter wait accepted non-exportable producer");
            invalid = bindings;
            invalid.Views[shared1].Native = invalid.Views[shared0].Native;
            Reject([&] { executor.TrySubmit(graph, invalid); }, "Foreign native resource view accepted");
            Require(executor.PendingCount() == 0 && first.GetCommandQueue()->FenceValue == firstBefore &&
                secondCopy.FenceValue == secondBefore, "Cross-adapter preflight failure submitted work");
        }
        const auto receipt = executor.TrySubmit(graph, bindings);
        Require(receipt && receipt->Batches.size() == 7 && receipt->Batches[1].Timeline() == firstCopy.GetTimelineIdentity() &&
            receipt->Batches[2].Timeline() == secondCopy.GetTimelineIdentity() &&
            receipt->Batches[3].Timeline() == compute->GetTimelineIdentity(), "Physical round-trip receipt has wrong placement");
    }
    Require(executor.PendingCount() == 2, "Gated physical invocations did not remain in flight");
    holdSecondCopy.Open();
    executor.Drain();
    CheckPayload(*readbacks[0], 73); CheckPayload(*readbacks[1], 74);
    RemoteExactAndReuse(first, second, firstCopy, secondCopy);
    std::cout << "Physical adapters " << firstLuid.HighPart << ':' << firstLuid.LowPart << " and "
        << secondLuid.HighPart << ':' << secondLuid.LowPart << ": two queued HAO round trips, allocation aliases and generations passed.\n";
}

static void CheckDebug(ID3D12InfoQueue& info)
{
    for (UINT64 i = 0; i < info.GetNumStoredMessages(); ++i)
    {
        SIZE_T size = 0;
        ThrowIfFailed(info.GetMessage(i, nullptr, &size));
        std::vector<BYTE> storage(size);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        ThrowIfFailed(info.GetMessage(i, message, &size));
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) throw std::runtime_error(message->pDescription);
    }
}

static bool RunPhysical(GDX12Device& primary)
{
    auto& factory = GDX12DeviceFactory::GetFactory();
    const auto primaryLuid = primary.GetAdapterLuid();
    for (UINT i = 0;; ++i)
    {
        ComPtr<IDXGIAdapter4> adapter;
        const auto hr = factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
        if (hr == DXGI_ERROR_NOT_FOUND) return false;
        ThrowIfFailed(hr);
        DXGI_ADAPTER_DESC1 desc{};
        ThrowIfFailed(adapter->GetDesc1(&desc));
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ||
            (desc.AdapterLuid.HighPart == primaryLuid.HighPart && desc.AdapterLuid.LowPart == primaryLuid.LowPart)) continue;
        const auto support = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device14), nullptr);
        if (support == DXGI_ERROR_UNSUPPORTED || support == E_NOTIMPL) continue;
        ThrowIfFailed(support);
        GDX12Device secondary;
        secondary.Role = DEVICE_ROLE_SECONDARY;
        ThrowIfFailed(secondary.Initialize(adapter));
        ComPtr<ID3D12InfoQueue> info;
        ThrowIfFailed(secondary.GetDevice().As(&info));
        info->ClearStoredMessages();
        TwoAdapters(primary, secondary);
        secondary.Reset();
        CheckDebug(*info.Get());
        return true;
    }
}
}

namespace LegacyBridgeTests
{
using namespace GDX12Submission;

class LinkedPass final : public GDX12RenderPass
{
public:
    using Recorder = std::function<void(GDX12CommandList*)>;
    LinkedPass(const char* name, std::vector<IRenderPassLink*> inputs, std::vector<IRenderPassLink*> outputs,
        Recorder record = {})
        : Name(name), Inputs(std::move(inputs)), Outputs(std::move(outputs)), Record(std::move(record)) {}
    void RecordRenderGraph(GDX12RenderGraphBuilder& builder) override
    {
        builder.SetName(Name);
        if (Live) builder.HasSideEffects();
        if (Async) builder.AsyncCompute();
        builder.ReadInputs(Inputs, D3D12_RESOURCE_STATE_COPY_SOURCE);
        for (auto* out : Outputs)
            builder.Write(out, Name, OutputLifetime, D3D12_RESOURCE_STATE_RENDER_TARGET);
        if (Forward.first) builder.Fallback(Forward.first, Forward.second);
    }
    void Execute(GDX12CommandList* list) override { if (Record) Record(list); }
    bool Live = true, Async = false;
    Graph::Lifetime OutputLifetime = Graph::Lifetime::Dynamic;
    std::pair<IRenderPassLink*, IRenderPassLink*> Forward{};
private:
    const char* Name;
    std::vector<IRenderPassLink*> Inputs, Outputs;
    Recorder Record;
};

static void Declarations()
{
    GDX12Texture input, other, first, second, unknown;
    LinkedPass producer("boundary sources", {}, {&input, &other});
    LinkedPass replaced("two-output replacement", {&input, &other}, {&first, &second});
    Graph graph;
    graph.AddPass(producer); graph.AddPass(replaced);
    Reject([&] { graph.SetLegacySplit(replaced, {&input}, {&first, &second}); }, "Missing boundary read accepted");
    Reject([&] { graph.SetLegacySplit(replaced, {&input, &other}, {&first}); }, "Missing boundary write accepted");
    Reject([&] { graph.SetLegacySplit(replaced, {&input, &other, &unknown}, {&first, &second}); }, "Unknown link accepted");
    Reject([&] { graph.SetLegacySplit(replaced, {&input, &other, &first}, {&first, &second}); }, "Write-only link accepted as input");
    Reject([&] { graph.SetLegacySplit(replaced, {&input, &other}, {&first, &first, &second}); }, "Duplicate output accepted");
    Reject([&] { graph.SetLegacyPrefixBefore(replaced, {}); }, "Prefix without enable predicate accepted");
    graph.SetLegacySplit(replaced, {&input, &other}, {&first, &second});
    graph.SetLegacySplit(replaced, {&other, &input}, {&second, &first});
    graph.CompilePlan();

    GDX12Texture unused;
    LinkedPass dead("culled split", {}, {&unused});
    dead.Live = false;
    Graph culled;
    culled.AddPass(dead);
    culled.SetLegacySplit(dead, {}, {&unused});
    Reject([&] { culled.CompilePlan(); }, "Split on a culled node accepted");
    std::cout << "Legacy split declaration validation passed.\n";
}

static void Pooling(GDX12Device& device)
{
    GDX12DeviceResources resources;
    resources.Initialize(&device);
    GDX12Texture scratch, input, alternate, output;
    for (auto* texture : {&scratch, &input, &alternate, &output})
        GDX12RenderHelpers::InitializeRenderTarget(*texture, resources, 4, 4, DXGI_FORMAT_R8G8B8A8_UNORM);
    LinkedPass scratchWriter("early scratch", {}, {&scratch}), scratchReader("retire scratch", {&scratch}, {});
    LinkedPass produce("boundary input", {}, {&input}), fallback("forwarded candidate", {}, {&alternate});
    LinkedPass replaced("replacement", {&input}, {&output}), tail("consume boundary", {&output}, {});
    replaced.Forward = {&input, &alternate};
    for (auto* pass : {&scratchWriter, &produce, &fallback, &replaced})
        pass->OutputLifetime = Graph::Lifetime::Transient;
    Graph graph(&device);
    for (auto* pass : {&scratchWriter, &scratchReader, &produce, &fallback, &replaced, &tail}) graph.AddPass(*pass);
    graph.Compile();
    Require(scratch.GetResource() == input.GetResource(), "Pooling control did not share disjoint lifetimes");
    graph.SetLegacySplit(replaced, {&input}, {&output});
    graph.Compile();
    Require(graph.GetStatistics().PhysicalTextures == 4 && scratch.GetResource() != input.GetResource() &&
        scratch.GetResource() != alternate.GetResource() && scratch.GetResource() != output.GetResource(),
        "Legacy split did not isolate the inputs, outputs and fallback candidate");
}

static void PrefixOnly(GDX12Device& device)
{
    auto& direct = *device.GetCommandQueue();
    unsigned producers = 0, readers = 0, cuts = 0;
    bool enabled = true, submit = true;
    LinkedPass producer("prefix producer", {}, {}, [&](GDX12CommandList*) { ++producers; });
    LinkedPass reader("prefix reader", {}, {}, [&](GDX12CommandList*)
        { Require(producers == readers + 1, "Prefix reader lost its original producer order"); ++readers; });
    Graph graph(&device);
    const auto producerNode = graph.AddPass(producer), readerNode = graph.AddPass(reader);
    graph.DependsOn(readerNode, producerNode);
    graph.SetLegacyPrefixBefore(reader, [&] { return enabled; });
    graph.Compile();
    const auto run = [&]
    {
        auto* tail = graph.ExecuteLegacySplit(direct.GetCommandList(), nullptr,
            [&](const Graph::LegacyBoundary& boundary, CompletionPoint prefix) -> std::optional<CompletionPoint>
            {
                ++cuts;
                Require(boundary.Replaced == &reader && boundary.Inputs.empty() && boundary.Outputs.empty() &&
                    boundary.ExternalViews.empty() && prefix.IsValid() && producers == readers + 1,
                    "Ordering-only cut should issue its prefix before the retained reader");
                if (!submit) return std::nullopt;
                direct.ExecuteCommandList(direct.GetCommandList());
                return direct.GetLastCompletionPoint();
            });
        direct.ExecuteCommandList(tail);
        direct.Flush();
    };
    run();
    submit = false;
    run();
    enabled = false;
    run();
    Require(producers == 3 && readers == 3 && cuts == 2, "Prefix cut skipped a reader or ran while disabled");
    std::cout << "Ordering-only legacy prefix preserved the actual reader on submit, fallback and disable.\n";
}

static void OptionalLoans(GDX12Device& device)
{
    GDX12DeviceResources resources;
    resources.Initialize(&device);
    GDX12Texture output;
    GDX12RenderHelpers::InitializeRenderTarget(output, resources, 4, 4, DXGI_FORMAT_R8G8B8A8_UNORM);
    unsigned legacy = 0, placed = 0;
    LinkedPass pass("optional shadow atlas writer", {}, {&output}, [&](auto*) { ++legacy; });
    Graph graph(&device);
    graph.AddPass(pass);
    bool selected = false;
    graph.SetOptionalLegacySplit(pass, {}, {&output}, [&]() -> std::optional<std::vector<Graph::LegacyExternalView>>
    {
        if (!selected) return std::nullopt;
        return std::vector<Graph::LegacyExternalView>{};
    });
    graph.Compile();
    auto* direct = device.GetCommandQueue();
    for (const bool active : {false, true})
    {
        selected = active;
        auto* tail = graph.ExecuteLegacySplit(direct->GetCommandList(), nullptr,
            [&](const Graph::LegacyBoundary& boundary, CompletionPoint prefix)
            {
                Require(boundary.Outputs.size() == 1 && boundary.ExternalViews.empty() && prefix.IsValid(),
                    "Optional empty inventory lost its linked atlas");
                ++placed;
                direct->ExecuteCommandList(direct->GetCommandList());
                return std::optional<CompletionPoint>{direct->GetLastCompletionPoint()};
            });
        direct->ExecuteCommandList(tail);
    }
    direct->Flush();
    Require(legacy == 1 && placed == 1, "Null and empty legacy inventories must have distinct behavior");
}

static void Frame(GDX12Device& device, bool late)
{
    auto* direct = device.GetCommandQueue();
    auto* compute = device.TryGetComputeQueue();
    auto* copy = device.TryGetCopyQueue();
    Require(direct && compute && copy, "Legacy bridge fixture requires primary DIRECT, COMPUTE and COPY");
    const auto adapter = device.GetSubmissionAdapterId();
    GDX12DeviceResources resources;
    resources.Initialize(&device);
    GDX12Texture depth, earlyNormals, lateNormals, visibility, auxiliary, filtered;
    for (auto* texture : {&depth, &earlyNormals, &lateNormals, &visibility, &auxiliary, &filtered})
        GDX12RenderHelpers::InitializeRenderTarget(*texture, resources, 4, 4, DXGI_FORMAT_R8G8B8A8_UNORM);
    auto* normals = late ? &lateNormals : &earlyNormals;
    const auto clear = [](GDX12Texture& texture, std::array<float, 4> color)
    {
        return [&texture, color](GDX12CommandList* list)
        { list->GetCommandList()->ClearRenderTargetView(texture.GetRTV()->CPUHandle, color.data(), 0, nullptr); };
    };
    LinkedPass makeDepth("Depth output", {}, {&depth}, clear(depth, {1, 0, 0, 1}));
    LinkedPass makeEarly("Early normals", {}, {&earlyNormals}, clear(earlyNormals, {0, 1, 0, 1}));
    LinkedPass makeLate("Late opaque normals", {}, {&lateNormals}, clear(lateNormals, {0, 0, 1, 1}));
    unsigned firstLegacy = 0, secondLegacy = 0;
    LinkedPass first("replace AO pair", {&depth, normals}, {&visibility, &auxiliary},
        [&](GDX12CommandList*) { ++firstLegacy; });
    const auto clearFiltered = clear(filtered, {1, 1, 0, 1});
    LinkedPass second("fallback filter", {&visibility}, {&filtered},
        [&](GDX12CommandList* list) { ++secondLegacy; clearFiltered(list); });
    const auto desc = depth.GetResource()->D3DResource->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[4]{};
    UINT64 end = 0;
    for (auto& footprint : footprints)
    {
        const auto offset = (end + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) & ~(UINT64(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT) - 1);
        device.GetDevice()->GetCopyableFootprints(&desc, 0, 1, offset, &footprint, nullptr, nullptr, nullptr);
        end = footprint.Offset + footprint.Footprint.RowPitch * footprint.Footprint.Height;
    }
    auto readback = device.CreateResource(D3D12_HEAP_TYPE_READBACK, CD3DX12_RESOURCE_DESC::Buffer(end), D3D12_RESOURCE_STATE_COPY_DEST);
    LinkedPass prefixReader("legacy async prefix reader", {normals}, {},
        [&](GDX12CommandList* list)
        {
            Require(list->GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE, "Legacy prefix reader lost its COMPUTE placement");
            const CD3DX12_TEXTURE_COPY_LOCATION src(normals->GetResource()->D3DResource.Get(), 0);
            const CD3DX12_TEXTURE_COPY_LOCATION dst(readback.Get(), footprints[3]);
            list->GetCommandList()->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        });
    prefixReader.Async = true;
    LinkedPass consumer("legacy async tail reader", {&visibility, &auxiliary, &filtered}, {},
        [&](GDX12CommandList* list)
        {
            Require(list->GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE, "Legacy tail reader lost its COMPUTE placement");
            GDX12Texture* outputs[]{&visibility, &auxiliary, &filtered};
            for (size_t i = 0; i < 3; ++i)
            {
                const CD3DX12_TEXTURE_COPY_LOCATION src(outputs[i]->GetResource()->D3DResource.Get(), 0);
                const CD3DX12_TEXTURE_COPY_LOCATION dst(readback.Get(), footprints[i]);
                list->GetCommandList()->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            }
        });
    consumer.Async = true;
    Graph graph(&device);
    for (auto* pass : {&makeDepth, &makeEarly, &makeLate, &prefixReader, &first, &second, &consumer}) graph.AddPass(*pass);
    graph.SetLegacySplit(second, {&visibility}, {&filtered}); // Register out of execution order.
    graph.SetLegacySplit(first, {&depth, normals}, {&visibility, &auxiliary});
    graph.SetLegacySplit(second, {&visibility}, {&filtered}); // Update rather than duplicate.
    graph.Compile();

    ComPtr<ID3D12Fence> gateFence;
    Graph::PlanExecutor executor(1);
    PlacedTests::Gate gate(device, *copy, gateFence);
    std::vector<GDX12RenderPass*> visits;
    CompletionPoint finalized;
    auto* tail = graph.ExecuteLegacySplit(direct->GetCommandList(), compute,
        [&](const Graph::LegacyBoundary& boundary, CompletionPoint prefix) -> std::optional<CompletionPoint>
        {
            visits.push_back(boundary.Replaced);
            if (boundary.Replaced == &second)
            {
                Require(prefix.Value() > finalized.Value() && boundary.Inputs[0].Native.get() == visibility.GetResource(),
                    "Second split lost the first replacement's issued result");
                return std::nullopt;
            }
            Require(boundary.Replaced == &first && boundary.Inputs.size() == 2 && boundary.Outputs.size() == 2 &&
                boundary.Inputs[0].Link == &depth && boundary.Inputs[1].Link == normals &&
                boundary.Inputs[1].Native.get() == normals->GetResource() &&
                boundary.Outputs[0].Native.get() == visibility.GetResource() && prefix.Timeline() == direct->GetTimelineIdentity(),
                "Legacy boundary returned wrong frame links, views or timeline");

            Graph placed;
            Graph::ExecutionBindings bindings;
            bindings.Queues = {{&device, direct}, {&device, copy}};
            const auto d = PlacedTests::Register(placed, bindings, "depth", device, boundary.Inputs[0].Native, 1, 1, {}, true);
            const auto n = PlacedTests::Register(placed, bindings, "normals", device, boundary.Inputs[1].Native, 2, 2, {}, true);
            const auto staging = PlacedTests::Register(placed, bindings, "packed normals", device,
                PlacedTests::MakeResource(device, D3D12_HEAP_TYPE_DEFAULT, desc), 3, 3);
            const auto out = PlacedTests::Register(placed, bindings, "AO output", device, boundary.Outputs[0].Native, 4, 4, {}, true);
            const auto aux = PlacedTests::Register(placed, bindings, "AO auxiliary", device, boundary.Outputs[1].Native, 5, 5, {}, true);
            PlacedTests::Pass pack("same-DIRECT placed prefix", {adapter, QueueKind::Direct}, {},
                [](GDX12CommandList&, const Graph::BoundResources&) {});
            auto delay = PlacedTests::Copy("gated COPY", {adapter, QueueKind::Copy}, n, staging);
            PlacedTests::Pass finalize("same-DIRECT finalize", {adapter, QueueKind::Direct},
                {{d, Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE}, {staging, Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE},
                 {out, Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST}, {aux, Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST}},
                [=](GDX12CommandList& list, const Graph::BoundResources& views)
                { list.CopyResource(views.Get(out), views.Get(staging)); list.CopyResource(views.Get(aux), views.Get(d)); });
            const auto packId = placed.AddPass(pack), delayId = placed.AddPass(delay);
            placed.AddPass(finalize); placed.DependsOn(delayId, packId); placed.CompilePlan();
            const auto receipt = executor.TrySubmit(placed, bindings);
            Require(receipt && receipt->Batches.size() == 3 && !executor.IsSubmissionFaulted(), "Placed bridge invocation did not submit");
            finalized = receipt->Batches.back();
            PlacedTests::RequireReached(prefix, *direct);
            Require(!finalized.IsReachedBy(direct->GetCompletionProgress()), "Gated finalize finished before COPY was released");
            return finalized;
        });
    direct->ExecuteCommandList(tail);
    const auto tailPoint = direct->GetLastCompletionPoint();
    const bool orderValid = visits == std::vector<GDX12RenderPass*>{&first, &second} && firstLegacy == 0 && secondLegacy == 1 &&
        graph.GetStatistics().AsyncPasses == 2 && graph.GetStatistics().AsyncJoins == 2 &&
        tailPoint.Value() > finalized.Value() && !tailPoint.IsReachedBy(direct->GetCompletionProgress());
    gate.Open();
    direct->CPUWaitForFenceValue(tailPoint.Value());
    executor.Drain();
    Require(orderValid, "Replacement/fallback order, legacy COMPUTE joins or retirement point is wrong");

    const std::array<BYTE, 4> normalColor{0, BYTE(late ? 0 : 255), BYTE(late ? 255 : 0), 255};
    const std::array<std::array<BYTE, 4>, 4> expected{{
        normalColor, {255, 0, 0, 255}, {255, 255, 0, 255}, normalColor }};
    BYTE* mapped = nullptr;
    ThrowIfFailed(readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped)));
    bool equal = true;
    for (size_t i = 0; i < expected.size(); ++i)
        for (UINT y = 0; y < footprints[i].Footprint.Height; ++y)
            for (UINT x = 0; x < footprints[i].Footprint.Width; ++x)
            {
                const auto* pixel = mapped + footprints[i].Offset + y * footprints[i].Footprint.RowPitch + x * 4;
                equal &= std::equal(expected[i].begin(), expected[i].end(), pixel);
            }
    readback->Unmap(0, nullptr);
    Require(equal, "Legacy COMPUTE tail read wrong replacement/fallback contents");

    if (!late)
        for (bool staleReceipt : {true, false})
        {
            unsigned calls = 0;
            Reject([&] { graph.ExecuteLegacySplit(direct->GetCommandList(), compute,
                [&](const Graph::LegacyBoundary&, CompletionPoint prefix) -> std::optional<CompletionPoint>
                {
                    ++calls;
                    if (staleReceipt) return prefix;
                    direct->SignalCompletion();
                    return std::nullopt;
                }); }, staleReceipt ? "Legacy bridge accepted its own prefix as a replacement" :
                "Legacy bridge accepted fallback after the callback issued a DIRECT point");
            direct->Flush();
            Require(calls == 1 && firstLegacy == 0 && secondLegacy == 1,
                "Invalid replacement result ran the legacy node or a later callback");
        }
}

static void Run(GDX12Device& device)
{
    Pooling(device);
    Frame(device, false);
    Frame(device, true);
    std::cout << "Legacy bridge early/late links, two replacements, multi-output handoff, async readback and prefix progress passed.\n";
}
}

namespace LegacyLoanTests
{
using namespace GDX12Submission;

class BufferPass final : public TestPass
{
public:
    BufferPass(const char* name, std::function<void(GDX12CommandList*)> record, std::vector<Graph::BufferAccess> accesses = {})
        : TestPass(name, {}, true), Record(std::move(record)), Accesses(std::move(accesses)) {}
    void Execute(GDX12CommandList* list) override { Record(list); }
    std::vector<Graph::BufferAccess> GetBufferAccesses() const override { return Accesses; }
private:
    std::function<void(GDX12CommandList*)> Record;
    std::vector<Graph::BufferAccess> Accesses;
};

static void BufferOnlyDeclaration()
{
    Graph graph;
    BufferPass producer("buffer-only point", [](GDX12CommandList*) {});
    graph.AddPass(producer);
    Reject([&] { graph.SetLegacySplit(producer, {}, {}); }, "Boundary-less split accepted without explicit resolver");
    graph.SetLegacySplit(producer, {}, {}, [] { return std::vector<Graph::LegacyExternalView>{}; });
    graph.CompilePlan();
}

static void GeometryOwnership(GDX12Device& device)
{
    SubMesh triangle;
    triangle.Vertices.resize(3);
    triangle.Indices = {0, 1, 2};
    Mesh mesh({std::move(triangle)}, DirectX::BoundingBox{});
    GDX12GeometryBuffer geometry(&device);
    geometry.AddMesh(&mesh, MeshHandle{1});
    auto old = geometry.ShareVertexResource();
    auto oldIndex = geometry.ShareIndexResource();
    Require(old && old->D3DResource && old->D3DResource->GetGPUVirtualAddress() == geometry.GetVertexAddress(),
        "Mesh vertex binding did not use its canonical allocation");
    auto* direct = device.GetCommandQueue();
    auto* list = direct->GetCommandList();
    Require(geometry.BeginSkinning(list) == old->D3DResource.Get() &&
        old->GetCurrentState() == D3D12_RESOURCE_STATE_UNORDERED_ACCESS, "Skinning did not update the canonical vertex state");
    geometry.EndSkinning(list);
    Require(old->GetCurrentState() ==
        (D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        "Skinning did not restore the vertex read state");
    list->SetGeometryBuffer(&geometry);
    Require(oldIndex && oldIndex->GetCurrentState() == D3D12_RESOURCE_STATE_INDEX_BUFFER,
        "IA bind did not acquire the canonical index view");
    direct->ExecuteCommandList(list);
    auto* native = old->D3DResource.Get();
    auto* nativeIndex = oldIndex->D3DResource.Get();
    geometry.AddMesh(&mesh, MeshHandle{2});
    Require(geometry.ShareVertexResource() != old && old->D3DResource.Get() == native &&
        geometry.ShareIndexResource() != oldIndex && oldIndex->D3DResource.Get() == nativeIndex,
        "Geometry growth mutated an already-retained allocation");
    direct->Flush();
}

static void Frame(GDX12Device& device)
{
    auto* direct = device.GetCommandQueue();
    auto* compute = device.TryGetComputeQueue();
    auto* copy = device.TryGetCopyQueue();
    Require(direct && compute && copy, "Legacy loan fixture requires primary DIRECT, COMPUTE and COPY");
    const auto adapter = device.GetSubmissionAdapterId();
    auto owner = PlacedTests::MakeResource(device, D3D12_HEAP_TYPE_DEFAULT);
    auto* loan = owner.get();
    const std::weak_ptr<GDX12Resource> lifetime = owner;
    auto legacyUpload = PlacedTests::MakeResource(device, D3D12_HEAP_TYPE_UPLOAD);
    auto retainedUpload = std::make_shared<GDX12UploadBuffer<UINT>>(&device, UINT(PlacedTests::Bytes / sizeof(UINT)), EBufferType::Upload, false);
    auto uploadView = std::shared_ptr<GDX12Resource>(retainedUpload, &retainedUpload->GetResource());
    Require(uploadView->GetCurrentState() == D3D12_RESOURCE_STATE_GENERIC_READ, "UPLOAD tracker lacks its canonical fixed state");
    PlacedTests::Fill(*legacyUpload, 23);
    PlacedTests::Fill(*uploadView, 41);
    auto middleResult = PlacedTests::MakeResource(device, D3D12_HEAP_TYPE_READBACK);
    auto tailResult = PlacedTests::MakeResource(device, D3D12_HEAP_TYPE_READBACK);
    const ResourceIdentity loanId{ResourceId{101}, adapter, Epoch{1}, TransferId::Invalid, AllocationId{101}};
    const ResourceIdentity uploadId{ResourceId{102}, adapter, Epoch{1}, TransferId::Invalid, AllocationId{102}};
    const auto write = [&](GDX12CommandList* list)
    {
        list->ResourceBarrier({loan->GetCopyDestBarrier()});
        list->CopyResource(loan->D3DResource.Get(), legacyUpload->D3DResource.Get());
    };
    unsigned firstLocal = 0, secondLocal = 0, visits = 0;
    BufferPass prefix("legacy producer", write, {{loan, Mode::Write}});
    BufferPass first("placed external output", [&](GDX12CommandList*) { ++firstLocal; });
    BufferPass middle("use first replacement", [&](GDX12CommandList* list)
    {
        Require(loan->GetCurrentState() == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            "Success did not reacquire declared legacy state");
        list->ResourceBarrier({loan->GetCopySourceBarrier()});
        list->CopyResource(middleResult->D3DResource.Get(), loan->D3DResource.Get());
    }, {{loan, Mode::Read}});
    BufferPass second("legacy fallback", [&](GDX12CommandList* list)
    {
        ++secondLocal;
        Require(loan->GetCurrentState() == D3D12_RESOURCE_STATE_COPY_SOURCE,
            "No-submit fallback did not restore the actual post-prefix state");
        write(list);
    }, {{loan, Mode::Write}});
    BufferPass tail("legacy async reader", [&](GDX12CommandList* list)
    {
        Require(list->GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE, "Tail reader lost COMPUTE placement");
        list->ResourceBarrier({loan->GetCopySourceBarrier()});
        list->CopyResource(tailResult->D3DResource.Get(), loan->D3DResource.Get());
    }, {{loan, Mode::Read}});
    tail.Async = true;
    Graph graph(&device);
    const auto prefixId = graph.AddPass(prefix), firstId = graph.AddPass(first), midId = graph.AddPass(middle);
    const auto secondId = graph.AddPass(second), tailId = graph.AddPass(tail);
    graph.DependsOn(firstId, prefixId); graph.DependsOn(midId, firstId);
    graph.DependsOn(secondId, midId); graph.DependsOn(tailId, secondId);
    enum class Invalid { None, UploadWrite, MissingResume, DuplicateIdentity };
    Invalid invalid = Invalid::None;
    const auto resolve = [&]
    {
        std::vector<Graph::LegacyExternalView> views{
            {loanId, owner, Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE},
            {uploadId, uploadView, Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE, std::nullopt}};
        if (invalid == Invalid::UploadWrite)
        {
            views[1].Usage = Mode::Write;
            views[1].PlacedState = D3D12_RESOURCE_STATE_COPY_DEST;
            views[1].LegacyAfter = D3D12_RESOURCE_STATE_GENERIC_READ;
        }
        else if (invalid == Invalid::MissingResume) views[0].LegacyAfter.reset();
        else if (invalid == Invalid::DuplicateIdentity) views[1].Identity = loanId;
        return views;
    };
    graph.SetLegacySplit(first, {}, {}, resolve);
    graph.SetLegacySplit(second, {}, {}, resolve);
    graph.Compile();
    const auto before = direct->GetLastCompletionPoint();
    for (auto fault : {Invalid::UploadWrite, Invalid::MissingResume, Invalid::DuplicateIdentity})
    {
        invalid = fault;
        Reject([&] { graph.ExecuteLegacySplit(direct->GetCommandList(), compute,
            [](const Graph::LegacyBoundary&, CompletionPoint) -> std::optional<CompletionPoint> { return std::nullopt; }); },
            "Invalid external view reached a split prefix");
        Require(direct->GetLastCompletionPoint().Value() == before.Value(), "Invalid external view submitted legacy work");
    }
    invalid = Invalid::None;
    {
        auto original = loan->D3DResource;
        Reject([&] { graph.ExecuteLegacySplit(direct->GetCommandList(), compute,
            [&](const Graph::LegacyBoundary&, CompletionPoint) -> std::optional<CompletionPoint>
            {
                loan->D3DResource = middleResult->D3DResource;
                return std::nullopt;
            }); }, "Native view rebinding was accepted across a split");
        loan->D3DResource = std::move(original);
        direct->Flush();
        Require(firstLocal == 0 && secondLocal == 0, "Native view mutation invoked a local fallback");
    }

    ComPtr<ID3D12Fence> copyFence, computeFence, tailFence;
    Graph::PlanExecutor executor(1);
    PlacedTests::Gate gateCopy(device, *copy, copyFence);
    PlacedTests::Gate gateCompute(device, *compute, computeFence);
    CompletionPoint finalized;
    auto* finalList = graph.ExecuteLegacySplit(direct->GetCommandList(), compute,
        [&](const Graph::LegacyBoundary& boundary, CompletionPoint prefixPoint) -> std::optional<CompletionPoint>
        {
            ++visits;
            Require(boundary.Inputs.empty() && boundary.Outputs.empty() && boundary.ExternalViews.size() == 2 &&
                boundary.ExternalViews[0].Native.get() == loan && loan->GetCurrentState() == D3D12_RESOURCE_STATE_COMMON &&
                uploadView->GetCurrentState() == D3D12_RESOURCE_STATE_GENERIC_READ, "Wrong buffer-only boundary release");
            if (boundary.Replaced == &second)
            {
                Require(prefixPoint.Value() > finalized.Value(), "Second split lost the earlier replacement dependency");
                return std::nullopt;
            }
            Require(boundary.Replaced == &first, "Unexpected external-view split");
            Graph placed;
            Graph::ExecutionBindings bindings;
            bindings.Queues = {{&device, direct}, {&device, copy}};
            const auto in = PlacedTests::Register(placed, bindings, "fixed upload", device,
                boundary.ExternalViews[1].Native, 102, 102, {}, true);
            const auto out = PlacedTests::Register(placed, bindings, "loan", device,
                boundary.ExternalViews[0].Native, 101, 101, {}, true);
            PlacedTests::Pass pending("gated dependency", {adapter, QueueKind::Copy}, {},
                [](GDX12CommandList&, const Graph::BoundResources&) {});
            auto fill = PlacedTests::Copy("write legacy buffer", {adapter, QueueKind::Direct}, in, out);
            const auto producer = placed.AddPass(pending), consumer = placed.AddPass(fill);
            placed.DependsOn(consumer, producer); placed.CompilePlan();
            const auto issued = executor.TrySubmit(placed, bindings);
            Require(issued.has_value(), "Placed loan submission deferred");
            finalized = issued->Batches.back();
            PlacedTests::RequireReached(prefixPoint, *direct);
            Require(!finalized.IsReachedBy(direct->GetCompletionProgress()), "Loan callback bypassed its gated dependency");
            return finalized;
        });
    const auto beforeTail = direct->GetLastCompletionPoint();
    PlacedTests::Gate gateTail(device, *direct, tailFence);
    direct->ExecuteCommandList(finalList);
    const auto tailPoint = direct->GetLastCompletionPoint();
    owner.reset();
    gateCopy.Open();
    PlacedTests::RequireReached(beforeTail, *direct);
    const auto computePoint = compute->GetLastCompletionPoint();
    compute->CollectCompletedLists();
    Require(!lifetime.expired() && !computePoint.IsReachedBy(compute->GetCompletionProgress()),
        "The legacy loan did not survive a gated COMPUTE list");
    gateCompute.Open();
    PlacedTests::RequireReached(computePoint, *compute);
    executor.CollectCompleted();

    direct->CollectCompletedLists();
    compute->CollectCompletedLists();
    const bool retained = !lifetime.expired() && executor.PendingCount() == 0 &&
        !tailPoint.IsReachedBy(direct->GetCompletionProgress());
    gateTail.Open();
    PlacedTests::RequireReached(tailPoint, *direct);
    direct->CollectCompletedLists();
    Require(retained && lifetime.expired(), "The legacy loan did not retire at its actual DIRECT tail without list reuse");
    Require(visits == 2 && firstLocal == 0 && secondLocal == 1 && graph.GetStatistics().AsyncJoins == 1,
        "External-buffer replacement/fallback or tail order changed");
    PlacedTests::CheckPayload(*middleResult, 41);
    PlacedTests::CheckPayload(*tailResult, 23);
    std::cout << "Legacy external views: buffer-only, fixed UPLOAD, two splits, reacquire, fallback and DIRECT tail retention passed.\n";
}

static void Run(GDX12Device& device)
{
    GeometryOwnership(device);
    Frame(device);
}
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
        LegacyBridgeTests::Declarations();
        LegacyLoanTests::BufferOnlyDeclaration();
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
        bool streamline = false, placed = false, requireTwo = false;
        for (int i = 1; i < argc; ++i)
            if (std::string_view(argv[i]) == "--gpu-validation")
            {
                ComPtr<ID3D12Debug1> gbv;
                ThrowIfFailed(debug.As(&gbv));
                gbv->SetEnableGPUBasedValidation(TRUE);
            }
            else if (std::string_view(argv[i]) == "--streamline")
                streamline = true;
            else if (std::string_view(argv[i]) == "--placed")
                placed = true;
            else if (std::string_view(argv[i]) == "--require-two-adapters")
                placed = requireTwo = true;
        if (streamline) Require(GDX12StreamlineSDK::Get().Initialize(), "Streamline initialization unavailable for fixture");
        GDX12Device device;
        ThrowIfFailed(device.Initialize(GDX12DeviceFactory::GetMostPerformantAdapter()));
        ComPtr<ID3D12InfoQueue> info;
        ThrowIfFailed(device.GetDevice().As(&info));
        info->ClearStoredMessages();
        TestPooling(device);
        TestAsyncQueues(device);
        TestAsyncTexture(device);
        if (placed)
        {
            PlacedTests::LocalChain(device);
            PlacedTests::ExactAndRetirement(device);
            PlacedTests::TextureUAV(device);
            LegacyBridgeTests::Run(device);
            LegacyBridgeTests::PrefixOnly(device);
            LegacyBridgeTests::OptionalLoans(device);
            LegacyLoanTests::Run(device);
        }
        if (requireTwo)
        {
            Require(PlacedTests::RunPhysical(device), "Physical two-adapter acceptance requested but no compatible adapter found");
            std::cout << "Native graph physical two-GPU=PASS\n";
        }
        PlacedTests::CheckDebug(*info.Get());
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

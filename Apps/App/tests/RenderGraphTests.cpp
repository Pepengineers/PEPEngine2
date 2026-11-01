#include "App.Base/App.h"
#include "Engine.RendererDX12/GDX12RenderGraph.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include <d3d12sdklayers.h>
#include <iostream>
#include <sstream>
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
    }
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
        CD3DX12_TEXTURE_COPY_LOCATION source(Texture.GetResource()->D3DResource.Get(), 0);
        CD3DX12_TEXTURE_COPY_LOCATION destination(Readback, Footprint);
        commands->GetCommandList()->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    }
private:
    GDX12Texture& Texture;
    ID3D12Resource* Readback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT Footprint;
};

static void TestCompiler()
{
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

int main()
{
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    try
    {
        TestCompiler();
        TestPassRecording();
        TestApp app;
        ComPtr<ID3D12Debug> debug;
        ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
        debug->EnableDebugLayer();
        GDX12Device device;
        ThrowIfFailed(device.Initialize(GDX12DeviceFactory::GetMostPerformantAdapter()));
        ComPtr<ID3D12InfoQueue> info;
        ThrowIfFailed(device.GetDevice().As(&info));
        info->ClearStoredMessages();
        TestPooling(device);
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

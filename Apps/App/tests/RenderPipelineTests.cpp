#include "Rendering/Grass/BistroGrassGeometry.h"
#include "Rendering/BistroRenderPipeline.h"
#include "App.Base/Modules/RenderModule.h"
#include "App.Base/Rendering/DefaultRenderPipeline.h"
#include <iostream>
#include <stdexcept>

static void Require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

class LinkedPass final : public GDX12RenderPass
{
public:
    explicit LinkedPass(const char* name) : Name(name) { _numOutputs = 1; _outputs = { &Output }; }
    void RecordRenderGraph(GDX12RenderGraphBuilder& graph) override
    {
        graph.SetName(Name);
        graph.ReadInputs(_inputs);
        graph.Write(OutputLink, Name, GDX12RenderGraph::Lifetime::Dynamic);
        for (auto* output : ExtraOutputs) graph.Write(output, Name, GDX12RenderGraph::Lifetime::Dynamic);
        if (Export) graph.Export(OutputLink);
    }
    const char* Name;
    GDX12Texture Output;
    IRenderPassLink* OutputLink = &Output;
    std::vector<IRenderPassLink*> ExtraOutputs;
    bool Export = false;
};

// A second application adds a resource-producing pass without modifying any runtime source.
class TestPipeline final : public IRenderPipeline
{
public:
    explicit TestPipeline(bool insert) : Insert(insert)
    { Bindings.PrimaryPasses = { &Producer, &Consumer }; if (insert) Bindings.PrimaryPasses.push_back(&ApplicationPass); }
    void Initialize(RenderPipelineContext&) override {}
    bool PrepareFrame(RenderPipelineContext&, GDX12CommandList*) override { return false; }
    void Resize(RenderPipelineContext&) override {}
    const RenderPipelineBindings& GetBindings() const override { return Bindings; }
    void BuildGraph(GDX12RenderGraph& graph, const RenderPipelineGraphInputs&) override
    {
        graph.AddPass(Producer);
        auto* color = Producer.GetOutput(0);
        if (Insert)
        {
            ApplicationPass.SetInputs({ color });
            graph.AddPass(ApplicationPass);
            color = ApplicationPass.GetOutput(0);
        }
        Consumer.SetInputs({ color });
        Consumer.Export = true;
        graph.AddPass(Consumer);
    }
    LinkedPass Producer{ "Producer" }, ApplicationPass{ "Application color" }, Consumer{ "Consumer" };
    RenderPipelineBindings Bindings;
    bool Insert;
};

static void TestApplicationComposition()
{
    for (bool insert : { false, true })
    {
        auto pipeline = std::make_unique<TestPipeline>(insert);
        auto* builder = pipeline.get();
        RenderModule runtime(nullptr, nullptr, std::move(pipeline));
        for (int rebuild = 0; rebuild < 3; ++rebuild)
        {
            GDX12RenderGraph graph;
            builder->BuildGraph(graph, { nullptr });
            graph.Compile();
            const auto& order = graph.GetExecutionOrder();
            Require(order.size() == (insert ? 3 : 2) && graph.GetStatistics().CulledPasses == 0,
                "Application-owned links were lost or rebuild duplicated passes");
            Require(order.front() == 0 && order.back() == order.size() - 1,
                "Application pass did not remain between producer and consumer");
        }
    }
    RenderModule base(nullptr, nullptr);
    RenderModule bistro(nullptr, nullptr, std::make_unique<BistroRenderPipeline>(std::make_shared<SceneManagerModule>()));
    bool rejected = false;
    try { RenderModule invalid(nullptr, nullptr, nullptr); }
    catch (const std::invalid_argument&) { rejected = true; }
    Require(rejected, "A null pipeline was accepted");
    rejected = false;
    try { base.GetReconstructionStatus(); }
    catch (const std::logic_error&) { rejected = true; }
    Require(rejected, "An uninitialized pipeline reported successful reconstruction");
}

static void TestAmbientOcclusionInputs()
{
    CommonRenderPasses passes(nullptr);
    RenderPipelineCommonData common;
    passes.Depth.SetInputs({});
    passes.Opaque.SetInputs({ passes.Depth.GetOutput(2) });
    for (bool early : { true, false, true })
    {
        passes.BindAmbientOcclusion(common, early);
        Require(common.AOAfterDepthPrepass == early, "AO shader preparation did not follow selected inputs");
        LinkedPass prepass("Prepass"), opaque("Opaque"), sink("AO consumer");
        prepass.OutputLink = passes.Depth.GetOutput(2);
        prepass.ExtraOutputs = { passes.Depth.GetOutput(3) };
        opaque.OutputLink = passes.Opaque.GetOutput(3);
        opaque.SetInputs({ passes.Depth.GetOutput(2) });
        sink.SetInputs({ passes.AO.GetOutput(0) });
        sink.Export = true;
        GDX12RenderGraph graph;
        graph.ImportShared(GDX12RenderGraph::SharedResource::Scene, "Scene");
        graph.AddPass(prepass);
        if (early) graph.AddPass(passes.AO);
        graph.AddPass(opaque);
        if (!early) graph.AddPass(passes.AO);
        graph.AddPass(sink);
        graph.Compile();
        const auto& order = graph.GetExecutionOrder();
        const std::vector<GDX12RenderGraph::Pass> expected = early ?
            std::vector<GDX12RenderGraph::Pass>{ 0, 1, 3 } : std::vector<GDX12RenderGraph::Pass>{ 0, 1, 2, 3 };
        Require(order == expected, "AO consumed normals from the wrong producer");
    }
}

class GeometryContributor final : public IRenderGeometrySource
{
public:
    std::vector<GDX12GeometryContext> Views;
    UINT StaticViews = 0;
    void DrawGeometry(GDX12CommandList*, const GDX12GeometryContext& view) override
    { if (view.StaticOnly) ++StaticViews; else Views.push_back(view); }
    void DeclareGeometryResources(GDX12RenderGraphBuilder& graph) const override
    { graph.ReadWriteExternal(this, "test geometry scratch"); }
};

class GeometryPass final : public GDX12RenderPass
{
public:
    explicit GeometryPass(const GDX12DeviceResources& resources) : Resources(resources) {}
    void RecordRenderGraph(GDX12RenderGraphBuilder& graph) override
    { graph.SetName("Geometry"); Resources.DeclareGeometryResources(graph); }
private:
    const GDX12DeviceResources& Resources;
};

static void TestGeometryAndGrassLifetime()
{
    GDX12DeviceResources resources;
    GeometryContributor contributor;
    resources.GeometrySources.push_back(&contributor);
    GeometryPass depth(resources), shadow(resources);
    for (int rebuild = 0; rebuild < 2; ++rebuild)
    {
        GDX12RenderGraph graph;
        const auto first = graph.AddPass(depth), second = graph.AddPass(shadow);
        graph.Compile();
        Require(graph.GetExecutionOrder() == std::vector<GDX12RenderGraph::Pass>{ first, second },
            "Mutable extension scratch lost draw ordering or was culled");
    }
    GDX12GeometryContext view;
    view.Eye = Vector3(100, 200, 300);
    view.CameraConstants = 77;
    for (const auto stage : { GDX12GeometryStage::Depth, GDX12GeometryStage::DepthNormals,
        GDX12GeometryStage::GBuffer, GDX12GeometryStage::Forward, GDX12GeometryStage::Shadow,
        GDX12GeometryStage::ReflectionCapture })
    {
        view.Stage = stage;
        resources.DrawGeometry(nullptr, view);
    }
    Require(contributor.Views.size() == 6 && contributor.Views.back().Eye == view.Eye &&
        contributor.Views.back().CameraConstants == 77, "Geometry dispatch lost the current view");
    view.StaticOnly = true;
    resources.DrawGeometry(nullptr, view);
    Require(contributor.Views.size() == 6 && contributor.StaticViews == 1,
        "Static-only filter did not reach the application contributor");

    auto scenes = std::make_shared<SceneManagerModule>();
    RenderModule renderer(nullptr, nullptr);
    World world(WorldDesc{});
    {
        BistroGrassGeometry extension(scenes, renderer);
        scenes->OnWorldCreated.Broadcast(world);
        auto& ecs = world.GetECS();
        auto entity = ecs.CreateEntity();
        const auto id = entity.GetId();
        auto& first = entity.AddComponent<GrassRenderComponent>();
        const auto firstOwner = first._renderOwner;
        Require(firstOwner != 0, "Bistro did not assign a grass owner");
        entity.AddComponent<GrassRenderComponent>();
        Require(ecs.Get<GrassRenderComponent>(id)._renderOwner > firstOwner, "Replacement retained old GPU identity");
        const auto replacedOwner = ecs.Get<GrassRenderComponent>(id)._renderOwner;
        ecs.DestroyEntity(id);
        auto reused = ecs.CreateEntity();
        Require(reused.AddComponent<GrassRenderComponent>()._renderOwner > replacedOwner,
            "Entity reuse inherited old grass GPU identity");
        scenes->OnWorldDestroyed.Broadcast(world);
        Require(ecs.CreateEntity().AddComponent<GrassRenderComponent>()._renderOwner == 0,
            "World removal retained Bistro component listeners");
        scenes->OnWorldCreated.Broadcast(world);
    }
    Require(world.GetECS().CreateEntity().AddComponent<WindFieldComponent>()._renderOwner == 0,
        "Geometry destruction retained world listeners");
}


int main()
{
    try
    {
        TestApplicationComposition();
        TestAmbientOcclusionInputs();
        TestGeometryAndGrassLifetime();
        std::cout << "Render pipeline composition, AO rebinding, rebuild and grass lifetime checks passed.\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

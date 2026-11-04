#include "Rendering/Grass/BistroGrassRenderExtension.h"
#include "App.Base/Modules/RenderModule.h"
#include "Engine.RendererDX12/GDX12RenderExtension.h"
#include <iostream>
#include <stdexcept>

static void Require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

class IndependentPass final : public GDX12RenderPass
{
public:
    explicit IndependentPass(int value) : Value(value) {}
    void RecordRenderGraph(GDX12RenderGraphBuilder& graph) override
    {
        graph.SetName(std::to_string(Value));
        graph.HasSideEffects();
    }
    int Value;
};

class TestExtension final : public GDX12RenderExtension
{
public:
    explicit TestExtension(int value) : Value(value) {}
    void AfterOpaque(GDX12RenderExtensionBuildContext& context) override
    {
        context.AddPass<IndependentPass>(Value);
    }
    int Value;
};

static void TestStagesAndLinks()
{
    std::vector<std::unique_ptr<GDX12RenderPass>> passes;
    GDX12DeviceResources resources;
    RenderPipelineCommonData common;
    GDX12Texture opaqueDepth, opaqueMotion, replacementDepth;
    GDX12RenderExtensionBuildContext context(passes, &resources, nullptr, &common, &opaqueDepth, &opaqueMotion);
    Require(context.SceneDepth == &opaqueDepth && context.SceneMotion == &opaqueMotion,
        "Default frame links are not the opaque links");
    context.SceneDepth = &replacementDepth;
    Require(context.OpaqueDepth() == &opaqueDepth && context.OpaqueMotion() == &opaqueMotion,
        "Selecting scene depth changed the opaque inputs");

    context.AddPass<IndependentPass>(0);
    context.FinishStage();
    TestExtension first(1), second(2);
    first.AfterOpaque(context);
    second.AfterOpaque(context);
    context.FinishStage();
    context.AddPass<IndependentPass>(3);
    context.FinishStage();
    Require(passes.size() == 4, "Extension pass ownership was lost");
    for (int rebuild = 0; rebuild < 2; ++rebuild)
    {
        GDX12RenderGraph graph;
        std::unordered_map<GDX12RenderPass*, GDX12RenderGraph::Pass> ids;
        // Reverse recording makes this a test of stage dependencies, not insertion order.
        for (auto pass = passes.rbegin(); pass != passes.rend(); ++pass)
            ids.emplace(pass->get(), graph.AddPass(**pass));
        context.ApplyStageDependencies(graph, ids);
        graph.Compile();
        const auto& order = graph.GetExecutionOrder();
        Require(order.front() == ids.at(passes[0].get()) && order.back() == ids.at(passes[3].get()),
            "Independent extension passes escaped their pipeline stage");
        Require(passes.size() == 4, "Graph rebuild duplicated extension passes");
    }
    GDX12RenderExtension noOp;
    noOp.AfterOpaque(context);
    Require(passes.size() == 4, "No-op extension changed the pipeline");
    GDX12TransparencyTail tail;
    Require(!noOp.BuildTransparency(context, tail), "Default extension took over transparency");
}

class TransparencyProvider final : public GDX12RenderExtension
{
public:
    bool BuildTransparency(GDX12RenderExtensionBuildContext&, GDX12TransparencyTail&) override { return true; }
};

static void TestTransparencyOwnership()
{
    std::vector<std::unique_ptr<GDX12RenderPass>> passes;
    GDX12RenderExtensionBuildContext context(passes, nullptr, nullptr, nullptr);
    std::vector<std::unique_ptr<GDX12RenderExtension>> extensions;
    GDX12TransparencyTail tail;
    Require(!GDX12BuildExtensionTransparency(extensions, context, tail), "Empty extension list took over transparency");
    extensions.push_back(std::make_unique<GDX12RenderExtension>());
    extensions.push_back(std::make_unique<TransparencyProvider>());
    Require(GDX12BuildExtensionTransparency(extensions, context, tail), "Transparency provider was ignored");
    extensions.push_back(std::make_unique<TransparencyProvider>());
    bool rejected = false;
    try { GDX12BuildExtensionTransparency(extensions, context, tail); }
    catch (const std::logic_error&) { rejected = true; }
    Require(rejected, "Multiple transparency providers were accepted");
}

class GeometryContributor final : public GDX12RenderExtension
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
    resources.RenderExtensions.push_back(&contributor);
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
        BistroGrassRenderExtension extension(scenes, renderer);
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
        "Extension destruction retained world listeners");
}

int main()
{
    try
    {
        TestStagesAndLinks();
        TestTransparencyOwnership();
        TestGeometryAndGrassLifetime();
        RenderModule renderer(nullptr, nullptr);
        auto extension = std::make_unique<TestExtension>(4);
        auto* expected = extension.get();
        Require(renderer.RegisterRenderExtension(std::move(extension)) == expected,
            "Renderer did not retain the registered extension");
        bool rejected = false;
        try { renderer.RegisterRenderExtension(nullptr); }
        catch (const std::invalid_argument&) { rejected = true; }
        Require(rejected, "Null render extension was accepted");
        std::cout << "Render extension stages, links, ownership and rebuild checks passed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

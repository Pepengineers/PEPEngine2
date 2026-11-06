#include "Rendering/Grass/BistroGrassGeometry.h"
#include "Rendering/Water/BistroWaterRendering.h"
#include "Rendering/BistroRenderPipeline.h"
#include "App.Base/Modules/RenderModule.h"
#include "App.Base/Rendering/DefaultRenderPipeline.h"
#include "Engine.RendererDX12/RenderPasses/GDX12GeometryViewPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12GeometryBatchPass.h"
#include "PreparedShadowViewChecks.h"
#include <iostream>
#include <stdexcept>
#include <sstream>

static void Require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

class PreparedViewPass final : public GDX12GeometryViewPass
{
public:
    UINT GetGeometryViewCapacity() const override { return 3; }
    const GDX12GeometryContext* GetGeometryView(UINT slot) const override
    { return slot == 1 ? nullptr : &Views.at(slot); }
    void BeginGeometry(GDX12CommandList*) override { ++Begins; }
    void ExecuteGeometryView(GDX12CommandList*, UINT slot) override { Consumed.push_back(slot); }
    void BindGeometryViewTargets(GDX12CommandList*, UINT slot) override { Bound.push_back(slot); }
    void EndGeometry(GDX12CommandList*) override { ++Ends; }
    std::array<GDX12GeometryContext, 3> Views;
    std::vector<UINT> Consumed, Bound;
    UINT Begins = 0, Ends = 0;
};

static void TestRenderBounds()
{
    GDX12RenderBounds combined;
    Require(combined.Empty(), "An empty bounds reduction retained geometry");
    bool rejected = false;
    try { combined.Get(); } catch (const std::logic_error&) { rejected = true; }
    Require(rejected, "Empty bounds must not expose infinite extrema");
    const BoundingBox boxes[] = {
        {{100000000, -10000000, 7}, {0.25f, 0.001f, 200}},
        {{-200000000, 5000000, -500}, {12, 30000, 0.01f}},
        {{0,0,0}, {1e-8f,1e-8f,1e-8f}}, {{-45,5,7}, {0,0,0}},
        {{1e20f,-1e20f,1e20f}, {1e-6f,0.001f,0.25f}}
    };
    const auto contains = [](const BoundingBox& outer, const BoundingBox& inner)
    {
        const double a[] = {outer.Center.x, outer.Center.y, outer.Center.z};
        const double b[] = {inner.Center.x, inner.Center.y, inner.Center.z};
        const double x[] = {outer.Extents.x, outer.Extents.y, outer.Extents.z};
        const double y[] = {inner.Extents.x, inner.Extents.y, inner.Extents.z};
        for (size_t i=0; i<3; ++i) if (a[i]-x[i] > b[i]-y[i] || a[i]+x[i] < b[i]+y[i]) return false;
        return true;
    };
    for (const auto& box : boxes)
    {
        GDX12RenderBounds single; single.Add(box);
        Require(contains(single.Get(), box), "Bounds reduction lost a thin, distant extent");
        const auto one = single.Get();
        Require(one.Extents.x >= box.Extents.x && one.Extents.y >= box.Extents.y && one.Extents.z >= box.Extents.z,
            "Double endpoint rounding erased a distant sub-ULP extent");
        combined.Add(box);
    }
    const auto bounds = combined.Get();
    for (const auto& box : boxes) Require(contains(bounds, box), "Combined bounds excluded a contributor");
    GDX12RenderBounds reverse;
    for (size_t i=std::size(boxes); i>0; --i) reverse.Add(boxes[i-1]);
    const auto reversed = reverse.Get();
    Require(std::memcmp(&bounds, &reversed, sizeof(bounds)) == 0, "Bounds depend on reduction order");
    GDX12RenderBounds casters, captureOnly;
    casters.Add(boxes[0]); casters.Add(boxes[2]);
    captureOnly.Add(boxes[1]); captureOnly.Add(boxes[3]); captureOnly.Add(boxes[4]);
    casters.Merge(GDX12RenderBounds{});
    captureOnly.Merge(casters);
    const auto merged = captureOnly.Get();
    Require(std::memcmp(&bounds, &merged, sizeof(bounds)) == 0, "Caster/capture reduction changed the union");
}

static void TestPreparedViewPhases()
{
    PreparedViewPass pass;
    pass.Execute(nullptr);
    Require(pass.Begins == 1 && pass.Ends == 1 && pass.Consumed == std::vector<UINT>{ 0, 2 },
        "Prepared geometry executed an inactive view or repeated frame setup/finalization");
    pass.SetSplitGeometry(true);
    pass.Execute(nullptr);
    Require(pass.Begins == 2 && pass.Ends == 1 && pass.Consumed.size() == 2,
        "Split geometry head consumed views or finalized before its application preparation jobs");
    for (UINT slot = 0; slot < pass.GetGeometryViewCapacity(); ++slot)
    {
        if (!pass.GetGeometryView(slot)) continue;
        pass.ExecuteGeometryView(nullptr, slot);
        pass.BindGeometryViewTargets(nullptr, slot);
    }
    pass.EndGeometry(nullptr);
    Require(pass.Begins == 2 && pass.Ends == 2 && pass.Consumed == std::vector<UINT>{ 0, 2, 0, 2 } &&
        pass.Bound == std::vector<UINT>{ 0, 2 }, "Split view jobs lost their prepared selection or target rebinding");
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

static void TestWaterLifetime()
{
    auto scenes = std::make_shared<SceneManagerModule>();
    World first(WorldDesc{}), second(WorldDesc{});
    {
        BistroWaterRendering water(scenes);
        scenes->OnWorldCreated.Broadcast(first);
        scenes->OnWorldCreated.Broadcast(second);
        auto create = [](World& world)
        {
            auto entity = world.GetECS().CreateEntity();
            entity.AddComponent<TransformComponent>(Vector3(10, 20, 30));
            entity.AddComponent<FluidRenderComponent>().SetParticles({ { Vector3::Zero, Vector3::Zero } });
            return entity.GetId();
        };
        auto entity = create(first);
        create(second);
        first.SetTimeScale(0.5f);
        second.SetPaused(true);
        auto snapshots = water.CollectScene(0.2f);
        Require(snapshots.size() == 2 && snapshots[0].DeltaSeconds == 0.1f && snapshots[1].DeltaSeconds == 0,
            "Water extraction lost world pause/time scale or a world");
        Require(snapshots[0].ContainerToWorld.Translation() == Vector3(10, 20, 30), "Water extraction lost the container transform");
        const auto owner = snapshots[0].Id;
        const auto author = snapshots[0].Component;
        const auto seed = snapshots[0].Component.GetParticles();
        auto& ecs = first.GetECS();
        ecs.DestroyEntity(entity);
        entity = create(first);
        static_cast<GDX12FluidComponent&>(ecs.Get<FluidRenderComponent>(entity)) = author;
        snapshots = water.CollectScene(0);
        Require(snapshots[0].Id > owner && snapshots[0].Component.GetParticles() == seed && seed->size() == 1,
            "Entity reuse inherited old water GPU identity or mutated an immutable snapshot");
        const auto reusedOwner = snapshots[0].Id;
        ecs.Add<FluidRenderComponent>(entity);
        Require(ecs.Get<FluidRenderComponent>(entity)._renderOwner > reusedOwner,
            "Component replacement retained old water GPU identity");
        scenes->OnWorldDestroyed.Broadcast(first);
        Require(water.CollectScene(0).size() == 1, "Removed world retained water snapshots");
        Require(ecs.CreateEntity().AddComponent<FluidRenderComponent>()._renderOwner == 0,
            "Removed world retained water listeners");
    }
    Require(second.GetECS().CreateEntity().AddComponent<FluidRenderComponent>()._renderOwner == 0 &&
        scenes->OnWorldCreated.IsEmpty() && scenes->OnWorldDestroyed.IsEmpty(),
        "Water owner destruction retained world listeners");
}

static void TestWaterGraphLinks()
{
    GDX12FluidSurfacePass surface;
    GDX12FluidResolvePass resolve(&surface);
    LinkedPass depth("Opaque depth"), motion("Opaque motion"), environment("Environment"), hdr("Behind HDR"), sink("Water consumer");
    surface.SetInputs({ depth.GetOutput(0), motion.GetOutput(0) });
    resolve.SetInputs({ hdr.GetOutput(0), environment.GetOutput(0) });
    sink.SetInputs({ resolve.GetOutput(0), surface.GetOutput(1) });
    sink.Export = true;
    Require(!surface.GetOutput(2)->GetTexture() && !surface.GetOutput(3)->GetTexture() && resolve.GetNumInputs() == 2,
        "Empty water eagerly allocated internal targets or made them lifecycle inputs");
    for (UINT rebuild = 0; rebuild < 2; ++rebuild)
    {
        GDX12RenderGraph graph;
        graph.ImportShared(GDX12RenderGraph::SharedResource::Scene, "Scene");
        graph.ImportShared(GDX12RenderGraph::SharedResource::ReflectionProbes, "Reflection cache");
        for (auto* pass : std::vector<GDX12RenderPass*>{ &depth, &motion, &environment, &hdr, &surface, &resolve, &sink }) graph.AddPass(*pass);
        graph.Compile();
        Require(graph.GetStatistics().Passes == 7 && graph.GetStatistics().CulledPasses == 0,
            "Water internal/fallback reads lost an opaque producer or rebuild duplicated passes");
    }
    for (auto* omitted : { surface.GetOutput(2), surface.GetOutput(3) })
    {
        GDX12RenderGraph missing;
        missing.ImportShared(GDX12RenderGraph::SharedResource::Scene, "Scene");
        missing.ImportShared(GDX12RenderGraph::SharedResource::ReflectionProbes, "Reflection cache");
        missing.ImportLink("HDR", hdr.GetOutput(0));
        missing.ImportLink("Environment", environment.GetOutput(0));
        missing.ImportLink("Opaque depth", depth.GetOutput(0));
        missing.ImportLink("Selected depth", surface.GetOutput(0));
        for (auto* internal : { surface.GetOutput(2), surface.GetOutput(3) })
            if (internal != omitted) missing.ImportLink("Other internal texture", internal);
        bool rejected = false;
        try { missing.AddPass(resolve); } catch (const std::invalid_argument&) { rejected = true; }
        Require(rejected, "Water resolve accepted an undeclared geometry/owner producer");
    }
}

class GeometryContributor final : public IRenderGeometrySource
{
public:
    mutable std::vector<GDX12GeometryContext> Views;
    mutable UINT StaticViews = 0;
    void GatherBatches(const GDX12GeometryContext& view, std::vector<GDX12RenderBatch>&) const override
    { if (view.Filter == GDX12GeometryFilter::Static) ++StaticViews; else Views.push_back(view); }
};

class TargetViewPass final : public GDX12GeometryViewPass
{
public:
    void RecordRenderGraph(GDX12RenderGraphBuilder& graph) override
    { graph.SetName("Geometry targets"); graph.Write(&Output, "Geometry result", Lifetime::Dynamic); }
    UINT GetGeometryViewCapacity() const override { return 2; }
    const GDX12GeometryContext* GetGeometryView(UINT slot) const override { return &Views.at(slot); }
    void BeginGeometry(GDX12CommandList*) override {}
    void ExecuteGeometryView(GDX12CommandList*, UINT) override {}
    void BindGeometryViewTargets(GDX12CommandList*, UINT) override {}
    void EndGeometry(GDX12CommandList*) override {}
    std::array<GDX12GeometryContext, 2> Views;
    GDX12Texture Output;
};

static void TestAliasedGeometryOrdering()
{
    GDX12DeviceResources resources;
    GeometryContributor source;
    TargetViewPass target;
    GDX12GeometryViewJobPass mesh(target, 0);
    LinkedPass prepareA("Prepare A"), prepareB("Prepare B"), consumer("Output consumer");
    GDX12GeometryBatchPass consumeA(resources, target, 0, source, 0, 1), consumeB(resources, target, 1, source, 0, 1);
    GDX12GeometryFinalizePass finish(target);
    consumer.SetInputs({ &target.Output });
    consumer.Export = true;
    GDX12RenderGraph graph;
    std::vector<GDX12RenderGraph::Pass> expected;
    for (auto* pass : std::vector<GDX12RenderPass*>{ &target, &mesh, &prepareA, &consumeA, &prepareB, &consumeB, &finish, &consumer })
    {
        const auto node = graph.AddPass(*pass);
        if (!expected.empty() && pass != &consumer) graph.DependsOn(node, expected.back());
        expected.push_back(node);
    }
    graph.Compile();
    Require(graph.GetExecutionOrder() == expected,
        "Aliased geometry reordered Prepare A / Consume A / Prepare B / Consume B or published targets before the tail");
}

static void TestGeometryAndGrassLifetime()
{
    GDX12DeviceResources resources;
    GeometryContributor contributor;
    resources.GeometrySources.push_back(&contributor);
    GDX12GeometryContext view;
    view.Eye = Vector3(100, 200, 300);
    view.CameraConstants = 77;
    for (const auto stage : { GDX12GeometryStage::Depth, GDX12GeometryStage::DepthNormals,
        GDX12GeometryStage::GBuffer, GDX12GeometryStage::Forward, GDX12GeometryStage::Shadow,
        GDX12GeometryStage::ReflectionCapture })
    {
        view.Stage = stage;
        std::vector<GDX12RenderBatch> batches;
        resources.GatherGeometryBatches(view, {}, batches);
    }
    Require(contributor.Views.size() == 6 && contributor.Views.back().Eye == view.Eye &&
        contributor.Views.back().CameraConstants == 77, "Geometry dispatch lost the current view");
    view.Filter = GDX12GeometryFilter::Static;
    std::vector<GDX12RenderBatch> batches;
    resources.GatherGeometryBatches(view, {}, batches);
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


struct RecordedDraws
{
    std::vector<GDX12DrawDescription> Draws;
    void DrawInstanced(UINT vertices, UINT instances, UINT startVertex, UINT startInstance)
    { GDX12DrawDescription d; d.Kind = GDX12DrawKind::Direct; d.Direct = { vertices, instances, startVertex, startInstance }; Draws.push_back(d); }
    void DrawIndexedInstanced(UINT indices, UINT instances, UINT startIndex, INT baseVertex, UINT startInstance)
    { GDX12DrawDescription d; d.Kind = GDX12DrawKind::Indexed; d.Indexed = { indices, instances, startIndex, baseVertex, startInstance }; Draws.push_back(d); }
    void ExecuteIndirect(ID3D12CommandSignature* signature, UINT maximum, ID3D12Resource* arguments, UINT64 argumentOffset,
        ID3D12Resource* count, UINT64 countOffset)
    {
        GDX12DrawDescription d; d.Signature = signature; d.MaxCommands = maximum; d.Arguments = arguments;
        d.ArgumentOffset = argumentOffset; d.Count = count; d.CountOffset = countOffset; Draws.push_back(d);
    }
};

class ExternalBatchBinding final : public IRenderBatchBinding
{
public:
    bool Supports(const GDX12GeometryContext& view, const GDX12RenderBatch&) const override
    { return view.Stage == GDX12GeometryStage::Shadow && view.DepthFormat == DXGI_FORMAT_D32_FLOAT && view.ColorTargetCount == 0; }
    void Bind(GDX12CommandList*) const override { ++Binds; }
    mutable UINT Binds = 0;
};

class ExternalBatchSource final : public IRenderGeometrySource
{
public:
    void GatherBatches(const GDX12GeometryContext& view, std::vector<GDX12RenderBatch>& batches) const override
    {
        LastView = view;
        if (view.Filter == GDX12GeometryFilter::Static) return;
        GDX12RenderBatch direct;
        direct.Binding = &Binding;
        direct.FirstInstance = 13;
        direct.InstanceCount = 7;
        direct.Draw.Kind = GDX12DrawKind::Direct;
        direct.Draw.Direct = { 6, Empty ? 0u : 7u, 3, 13 };
        batches.push_back(direct);
        GDX12RenderBatch indirect;
        indirect.Binding = &Binding;
        indirect.InstanceCount = 32;
        indirect.BufferAccesses = { { Empty ? nullptr : Resource, GDX12RenderGraph::Mode::Read } };
        indirect.Draw = { GDX12DrawKind::Indirect, {}, {}, reinterpret_cast<ID3D12CommandSignature*>(1),
            reinterpret_cast<ID3D12Resource*>(2), 48, reinterpret_cast<ID3D12Resource*>(3), 12, 32, 20 };
        if (Empty) indirect.Draw.MaxCommands = 0;
        batches.push_back(indirect);
    }
    std::vector<GDX12RenderGraph::BufferAccess> GetGeometryBufferAccesses(GDX12GeometryStage stage) const override
    { return DeclareResource && stage == GDX12GeometryStage::Shadow ? std::vector<GDX12RenderGraph::BufferAccess>{ { Resource, GDX12RenderGraph::Mode::Read } } : std::vector<GDX12RenderGraph::BufferAccess>{}; }
    mutable GDX12GeometryContext LastView;
    ExternalBatchBinding Binding;
    GDX12Resource* Resource = reinterpret_cast<GDX12Resource*>(4);
    bool DeclareResource = true;
    bool Empty = false;
};

static void TestPrimitiveRecords()
{
    GDX12DeviceResources resources;
    GDX12RenderPrimitive primitive;
    primitive.Bounds = { XMFLOAT3(1, 2, 3), XMFLOAT3(4, 5, 6) };
    primitive.FirstInstance = 37;
    primitive.GeometryRevision = 10;
    primitive.MaterialRevision = 20;
    primitive.TransformRevision = 30;
    resources.BeginMeshPrimitiveUpdate();
    const auto first = resources.PublishMeshPrimitive(37, primitive);
    resources.EndMeshPrimitiveUpdate();
    const auto revision = resources.Primitives.Get(first)->Revision();
    resources.BeginMeshPrimitiveUpdate();
    primitive.MaterialRevision = 21;
    const auto same = resources.PublishMeshPrimitive(37, primitive);
    resources.EndMeshPrimitiveUpdate();
    Require(first == same && resources.Primitives.Get(first)->Revision() != revision,
        "Material edits changed primitive identity or failed to invalidate metadata");
    const auto materialRevision = primitive.Revision();
    ++primitive.GeometryRevision;
    Require(primitive.Revision() != materialRevision, "Geometry edits retained primitive revision");
    primitive.Dynamic = true;
    Require(primitive.Matches(GDX12GeometryFilter::All) && primitive.Matches(GDX12GeometryFilter::Dynamic) &&
        !primitive.Matches(GDX12GeometryFilter::Static), "Primitive static/dynamic filtering changed");
    resources.RemoveMeshPrimitive(37);
    Require(resources.Primitives.Get(first) == nullptr, "Removed instance retained a primitive");
    resources.BeginMeshPrimitiveUpdate();
    const auto reused = resources.PublishMeshPrimitive(37, primitive);
    resources.EndMeshPrimitiveUpdate();
    Require(reused.Index == first.Index && reused.Generation != first.Generation && !resources.Primitives.Get(first),
        "Reused instance resurrected an old primitive handle");
    bool rejected = false;
    try { resources.Primitives.Update(first, primitive); } catch (const std::invalid_argument&) { rejected = true; }
    Require(rejected, "Stale primitive update was accepted");
    primitive.Visible = false;
    resources.BeginMeshPrimitiveUpdate();
    Require(resources.PublishMeshPrimitive(37, primitive) == reused, "Visibility changed primitive identity");
    resources.EndMeshPrimitiveUpdate();
    Require(!resources.Primitives.Get(reused)->Visible, "Invisible primitive lost participation state");
    resources.BeginMeshPrimitiveUpdate();
    resources.EndMeshPrimitiveUpdate();
    Require(!resources.Primitives.Get(reused), "Missing extraction retained stale primitive bounds");
    resources.RemoveMeshPrimitive(9000);
    resources.BeginMeshPrimitiveUpdate();
    const auto sparse = resources.PublishMeshPrimitive(4096, primitive);
    const auto low = resources.PublishMeshPrimitive(3, primitive);
    resources.EndMeshPrimitiveUpdate();
    resources.BeginMeshPrimitiveUpdate();
    Require(resources.PublishMeshPrimitive(4096, primitive) == sparse, "Sparse GPU instance changed primitive identity");
    resources.EndMeshPrimitiveUpdate();
    Require(resources.Primitives.Get(sparse) && !resources.Primitives.Get(low),
        "Retiring a missing low instance removed a retained high instance");
    rejected = false;
    try { resources.PublishMeshPrimitive(UINT_MAX, primitive); } catch (const std::invalid_argument&) { rejected = true; }
    Require(rejected, "Invalid GPU instance index was accepted");
}

static void TestBatchContract()
{
    GDX12DeviceResources resources;
    ExternalBatchSource external;
    resources.GeometrySources.push_back(&external);
    GDX12RenderPrimitive externalPrimitive;
    externalPrimitive.Source = &external;
    externalPrimitive.Dynamic = true;
    externalPrimitive.FirstInstance = 13;
    externalPrimitive.InstanceCount = 7;
    const auto externalHandle = resources.Primitives.Add(externalPrimitive);
    Require(resources.Primitives.Get(externalHandle)->Source == &external &&
        resources.Primitives.Get(externalHandle)->Matches(GDX12GeometryFilter::Dynamic),
        "External source could not publish its shared primitive/instance range");
    GDX12GeometryContext view;
    view.Stage = GDX12GeometryStage::Shadow;
    view.DepthFormat = DXGI_FORMAT_D32_FLOAT;
    view.Key = 0x12345678;
    view.FrameSlot = 2;
    view.Filter = GDX12GeometryFilter::Dynamic;
    view.Eye = Vector3(10, 20, 30);
    GDX12RenderBatch mesh;
    mesh.Draw = { GDX12DrawKind::Indirect, {}, {}, reinterpret_cast<ID3D12CommandSignature*>(5),
        reinterpret_cast<ID3D12Resource*>(6), 24, reinterpret_cast<ID3D12Resource*>(7), 8, 101, 24 };
    std::vector<GDX12RenderBatch> batches;
    resources.GatherGeometryBatches(view, mesh, batches);
    Require(batches.size() == 3 && external.Binding.Binds == 0, "Gathering issued graphics work or dropped a source");
    Require(external.LastView.Key == view.Key && external.LastView.FrameSlot == 2 && external.LastView.Eye == view.Eye &&
        external.LastView.Filter == view.Filter, "External source lost the prepared view/filter");
    Require(batches[1].FirstInstance == 13 && batches[1].InstanceCount == 7 && batches[2].Draw.ArgumentStride == 20 &&
        batches[0].Draw.ArgumentStride == 24, "Batch conversion changed instancing or indirect layouts");
    RecordedDraws recorded;
    for (const auto& batch : batches) GDX12EmitDraw(recorded, batch.Draw);
    Require(recorded.Draws.size() == 3 && recorded.Draws[0].MaxCommands == 101 && recorded.Draws[0].ArgumentOffset == 24 &&
        recorded.Draws[0].CountOffset == 8 && recorded.Draws[1].Direct.InstanceCount == 7 &&
        recorded.Draws[1].Direct.StartInstanceLocation == 13 && recorded.Draws[2].ArgumentOffset == 48 &&
        recorded.Draws[2].CountOffset == 12, "Common draw emission changed stream offsets or instance ranges");
    GDX12DrawDescription indexed;
    indexed.Kind = GDX12DrawKind::Indexed;
    indexed.Indexed = { 12, 4, 8, -3, 17 };
    GDX12EmitDraw(recorded, indexed);
    Require(recorded.Draws.back().Indexed.InstanceCount == 4 && recorded.Draws.back().Indexed.BaseVertexLocation == -3 &&
        recorded.Draws.back().Indexed.StartInstanceLocation == 17, "Indexed draw emission lost arguments");
    Require(external.Binding.Supports(view, batches[1]), "External binding rejected its supported view");
    view.Stage = GDX12GeometryStage::GBuffer;
    Require(!external.Binding.Supports(view, batches[1]), "Binding accepted incompatible stage/targets");
    bool rejected = false;
    try { GDX12EmitRenderBatch(nullptr, view, batches[1]); } catch (const std::invalid_argument&) { rejected = true; }
    Require(rejected && external.Binding.Binds == 0, "Incompatible batch reached graphics binding");
    GDX12RenderBatch empty;
    GDX12EmitRenderBatch(nullptr, view, empty);
    GDX12EmitDraw(recorded, empty.Draw);
    Require(recorded.Draws.size() == 4, "Zero maximum commands accessed null buffers or issued a draw");
    GDX12DrawDescription invalid;
    invalid.MaxCommands = 1;
    rejected = false;
    try { GDX12EmitDraw(recorded, invalid); } catch (const std::invalid_argument&) { rejected = true; }
    Require(rejected, "Nonempty indirect batch accepted missing resources");
    view.Stage = GDX12GeometryStage::Shadow;
    const auto accesses = resources.GetGeometryBufferAccesses(view.Stage);
    Require(accesses.size() == 1 && accesses[0].Resource == external.Resource && accesses[0].Usage == GDX12RenderGraph::Mode::Read,
        "Common consumer lost source physical accesses");
    view.Filter = GDX12GeometryFilter::Static;
    batches.clear();
    resources.GatherGeometryBatches(view, {}, batches);
    Require(batches.empty(), "Static-only capture retained dynamic external batches");
    view.Filter = GDX12GeometryFilter::Dynamic;
    external.DeclareResource = false;
    rejected = false;
    try { resources.GatherGeometryBatches(view, {}, batches); } catch (const std::invalid_argument&) { rejected = true; }
    Require(rejected, "Source batch omitted its physical read declaration");
    external.Empty = true;
    batches.clear();
    resources.GatherGeometryBatches(view, {}, batches);
    for (const auto& batch : batches) GDX12EmitDraw(recorded, batch.Draw);
    Require(recorded.Draws.size() == 4 && external.Binding.Binds == 0,
        "Empty external batches validated null physical reads or issued graphics work");
}


int main()
{
    try
    {
        TestPrimitiveRecords();
        TestRenderBounds();
        TestPreparedViewPhases();
        TestPreparedShadowViews();
        TestAliasedGeometryOrdering();
        TestBatchContract();
        TestApplicationComposition();
        TestAmbientOcclusionInputs();
        TestGeometryAndGrassLifetime();
        TestWaterLifetime();
        TestWaterGraphLinks();
        std::cout << "Render primitive/batch, pipeline composition, AO rebinding, rebuild and grass lifetime checks passed.\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

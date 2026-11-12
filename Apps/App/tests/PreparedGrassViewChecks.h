#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12GeometryBatchPass.h"

class PreparedGrassSource final : public IRenderGeometrySource
{
public:
    explicit PreparedGrassSource(GDX12GrassRenderer& renderer) : Renderer(renderer) {}
    void GatherBatches(const GDX12GeometryContext& view, std::vector<GDX12RenderBatch>& batches) const override
    { Renderer.GatherBatches(view, batches); }
    std::vector<GDX12RenderGraph::BufferAccess> GetGeometryBufferAccesses(GDX12GeometryStage) const override
    { return Renderer.DrawAccesses(); }
    GDX12GrassRenderer& Renderer;
};

class GrassTestViews final : public GDX12GeometryViewPass
{
public:
    explicit GrassTestViews(GDX12Texture& depth) : Depth(depth) {}
    void RecordRenderGraph(GDX12RenderGraphBuilder& graph) override
    { graph.SetName("Grass test views"); graph.Write(&Depth, "Grass test depth", Lifetime::Dynamic); graph.HasSideEffects(); }
    UINT GetGeometryViewCapacity() const override { return 2; }
    const GDX12GeometryContext* GetGeometryView(UINT slot) const override { return &Views.at(slot); }
    void BeginGeometry(GDX12CommandList*) override {}
    void ExecuteGeometryView(GDX12CommandList* commands, UINT slot) override
    { BindGeometryViewTargets(commands, slot); commands->ClearDepthStencilView(&Depth); }
    void BindGeometryViewTargets(GDX12CommandList* commands, UINT) override
    {
        commands->ResourceBarrier({ Depth.GetResource()->GetDepthWriteBarrier() });
        commands->SetViewport(Depth.GetViewport());
        commands->SetScissorRect(Depth.GetScissorRect());
        commands->SetRenderTargets({}, &Depth);
    }
    void EndGeometry(GDX12CommandList*) override {}
    GDX12Texture& Depth;
    std::array<GDX12GeometryContext, 2> Views;
};

class GrassTestPreparation final : public GDX12RenderPass
{
public:
    explicit GrassTestPreparation(GDX12GrassRenderer& renderer) : Renderer(renderer) {}
    void RecordRenderGraph(GDX12RenderGraphBuilder& graph) override { graph.SetName("Grass test preparation"); graph.HasSideEffects(); }
    void Execute(GDX12CommandList* commands) override { Renderer.PrepareGPU(commands); }
    std::vector<GDX12RenderGraph::BufferAccess> GetBufferAccesses() const override { return Renderer.PreparationAccesses(); }
    GDX12GrassRenderer& Renderer;
};

class GrassTestExpansion final : public GDX12RenderPass
{
public:
    GrassTestExpansion(GDX12GrassRenderer& renderer, GrassTestViews& views, UINT slot) : Renderer(renderer), Views(views), Slot(slot) {}
    void RecordRenderGraph(GDX12RenderGraphBuilder& graph) override { graph.SetName("Grass test expansion"); graph.HasSideEffects(); }
    void Execute(GDX12CommandList* commands) override { Renderer.Expand(commands, *Views.GetGeometryView(Slot), 0); }
    std::vector<GDX12RenderGraph::BufferAccess> GetBufferAccesses() const override
    { return Renderer.ExpansionAccesses(*Views.GetGeometryView(Slot), 0); }
    GDX12GrassRenderer& Renderer;
    GrassTestViews& Views;
    UINT Slot;
};

class GrassTestReadback final : public GDX12RenderPass
{
public:
    GrassTestReadback(GDX12Texture& depth, D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint) : Depth(depth), Footprint(footprint) {}
    void RecordRenderGraph(GDX12RenderGraphBuilder& graph) override { graph.SetName("Grass view readback"); graph.Read(&Depth); graph.HasSideEffects(); }
    void Execute(GDX12CommandList* commands) override
    {
        commands->ResourceBarrier({ Depth.GetResource()->GetCopySourceBarrier() });
        const CD3DX12_TEXTURE_COPY_LOCATION source(Depth.GetResource()->D3DResource.Get(), 0), destination(Target, Footprint);
        commands->GetCommandList()->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    }
    GDX12Texture& Depth;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT Footprint;
    ID3D12Resource* Target = nullptr;
};

static void TestPreparedGrassViews(GDX12Device& device, GDX12DeviceResources& resources,
    const RenderPipelineCommonData& common, GDX12Texture& depth, const Matrix& vp, Vector3 eye)
{
    auto* queue = device.GetCommandQueue();
    const UINT savedSlot = resources.CurrFrameConstantsIndex;
    auto frozenCommon = common;
    frozenCommon.ResetHistory = false;
    GDX12GrassRenderer renderer(&resources);
    PreparedGrassSource source(renderer);
    resources.GeometrySources.push_back(&source);
    GDX12GrassInstance grass;
    grass.Id = 7001;
    grass.RootsCm = { {0, 0, 0} };
    grass.Implementation = GDX12GrassImplementation::CrossedRibbons;
    grass.Segments = 3;
    grass.WidthCm = 12;
    grass.CaptureReflections = false;
    GDX12WindInstance wind;
    wind.Id = 7002;
    wind.Implementation = GDX12WindImplementation::Analytic;
    wind.DeltaSeconds = 1.0f / 60;
    grass.WindId = wind.Id;
    auto controls = GetGrassWindControls();
    controls.GrassEnabled = controls.WindEnabled = true;
    controls.GrassQuality = controls.WindQuality = 2;
    GrassTestViews views(depth);
    views.SetSplitGeometry(true);
    GrassTestPreparation prepare(renderer);
    GDX12GeometryViewJobPass beginA(views, 0), beginB(views, 1);
    GrassTestExpansion expandA(renderer, views, 0), expandB(renderer, views, 1);
    GDX12GeometryBatchPass consumeA(resources, views, 0, source, 0, 1), consumeB(resources, views, 1, source, 0, 1);
    GDX12GeometryFinalizePass finish(views);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT64 bytes = 0;
    const auto depthDesc = depth.GetResource()->D3DResource->GetDesc();
    device.GetDevice()->GetCopyableFootprints(&depthDesc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    GrassTestReadback readA(depth, footprint), readB(depth, footprint);
    std::vector<std::array<GDX12ResourceHandle, 2>> readbacks(resources.FrameConstants.size());
    GDX12RenderGraph graph(&device);
    GDX12RenderGraph::Pass previous = UINT_MAX;
    for (auto* pass : std::vector<GDX12RenderPass*>{ &prepare, &views, &beginA, &expandA, &consumeA, &readA,
        &beginB, &expandB, &consumeB, &readB, &finish })
    {
        const auto node = graph.AddPass(*pass);
        if (previous != UINT_MAX) graph.DependsOn(node, previous);
        previous = node;
    }
    graph.Compile(false);
    std::cout << "Prepared grass graph passes=" << graph.GetStatistics().Passes << " activeViews=2 indirectSubmissions=2 per frame.\n";
    GDX12RenderPrimitiveHandle firstHandle;
    uint64_t previousDeformation = 0;
    for (UINT slot = 0; slot < resources.FrameConstants.size(); ++slot)
    {
        resources.CurrFrameConstantsIndex = slot;
        renderer.BeginFrame(frozenCommon, {grass}, {wind}, controls);
        renderer.PublishPrimitives(resources.Primitives, &source);
        UINT primitiveCount = 0;
        resources.Primitives.ForEach([&](GDX12RenderPrimitiveHandle handle, const GDX12RenderPrimitive& primitive)
        {
            if (primitive.Source != &source) return;
            ++primitiveCount;
            if (slot == 0) firstHandle = handle;
            Require(handle == firstHandle && primitive.Dynamic && primitive.CastsShadows && !primitive.CapturedInReflections,
                "Grass primitive identity or shadow-only participation changed between frame slots");
            Require(!previousDeformation || previousDeformation != primitive.DeformationRevision,
                "Wind time failed to invalidate shadow-only primitive metadata before view selection");
            previousDeformation = primitive.DeformationRevision;
        });
        Require(primitiveCount == 1, "Grass recreated or duplicated its stable primitive record");
        views.Views[0] = GrassTestView(resources, GDX12GrassRenderer::Stage::Depth, vp, eye,
            resources.FrameConstants[slot]->CameraCB->GetElementAddress(0));
        views.Views[0].Key = 7010;
        views.Views[1] = views.Views[0];
        views.Views[1].Key = 7011;
        views.Views[1].ViewProjection = Matrix::CreateTranslation(100000, 0, 0) * vp;
        renderer.PrepareView(views.Views[0], 0);
        renderer.PrepareView(views.Views[1], 0);
        Require(renderer.ExpansionAccesses(views.Views[1], 0).empty(),
            "Off-frustum grass scheduled expansion buffers for this view");
        for (auto& readback : readbacks[slot]) readback = device.CreateResource(D3D12_HEAP_TYPE_READBACK,
            CD3DX12_RESOURCE_DESC::Buffer(bytes), D3D12_RESOURCE_STATE_COPY_DEST);
        readA.Target = readbacks[slot][0].Get();
        readB.Target = readbacks[slot][1].Get();
        auto* commands = queue->GetCommandList();
        commands = graph.Execute(commands);
        if (slot == 0)
        {
            renderer.Expand(commands, views.Views[0], 0);
            auto overlapping = views.Views[0];
            overlapping.Key = 7012;
            renderer.PrepareView(overlapping, 0);
            renderer.Expand(commands, overlapping, 0);
            bool rejected = false;
            try { std::vector<GDX12RenderBatch> batches; renderer.GatherBatches(views.Views[0], batches); }
            catch (const std::logic_error&) { rejected = true; }
            Require(rejected, "Grass allowed an earlier view to consume an overwritten aliased output");
            overlapping.ViewProjection = views.Views[1].ViewProjection;
            renderer.PrepareView(overlapping, 0);
            Require(renderer.ExpansionAccesses(overlapping, 0).empty(),
                "Reprepared off-frustum view retained an earlier visible grass output");
            auto staticView = views.Views[1];
            staticView.Filter = GDX12GeometryFilter::Static;
            std::vector<GDX12RenderBatch> staticBatches;
            renderer.GatherBatches(staticView, staticBatches);
            Require(staticBatches.empty(), "Static-only request reused dynamic grass prepared under the same view key");
        }
        queue->ExecuteCommandList(commands);
    }
    queue->Flush();
    for (auto& frame : readbacks)
        for (UINT view = 0; view < 2; ++view)
        {
            void* mapped = nullptr;
            const D3D12_RANGE range = {0, static_cast<SIZE_T>(bytes)};
            ThrowIfFailed(frame[view]->Map(0, &range, &mapped));
            bool coverage = false;
            for (UINT y = 0; y < footprint.Footprint.Height; ++y)
                for (UINT x = 0; x < footprint.Footprint.Width; ++x)
                    coverage |= *reinterpret_cast<const float*>(static_cast<const BYTE*>(mapped) +
                        footprint.Offset + y * footprint.Footprint.RowPitch + x * sizeof(float)) > 0;
            const D3D12_RANGE noWrite = {};
            frame[view]->Unmap(0, &noWrite);
            Require(coverage == (view == 0), "Queued view expansion overwrote a previous common-consumer draw");
        }
    // A single blade has deterministic output order, allowing exact frozen-position comparison.
    const auto depthView = views.Views[0];
    auto gbufferView = depthView;
    gbufferView.Stage = GDX12GeometryStage::GBuffer;
    renderer.PrepareView(gbufferView, 0);
    auto* commands = queue->GetCommandList();
    renderer.Expand(commands, depthView, 0);
    auto* vertices = renderer.DrawAccesses().front().Resource;
    const UINT64 vertexBytes = vertices->D3DResource->GetDesc().Width;
    std::array<GDX12ResourceHandle, 2> positions;
    for (auto& target : positions) target = device.CreateResource(D3D12_HEAP_TYPE_READBACK,
        CD3DX12_RESOURCE_DESC::Buffer(vertexBytes), D3D12_RESOURCE_STATE_COPY_DEST);
    commands->ResourceBarrier({ vertices->GetCopySourceBarrier() });
    commands->GetCommandList()->CopyBufferRegion(positions[0].Get(), 0, vertices->D3DResource.Get(), 0, vertexBytes);
    renderer.Expand(commands, gbufferView, 0);
    commands->ResourceBarrier({ vertices->GetCopySourceBarrier() });
    commands->GetCommandList()->CopyBufferRegion(positions[1].Get(), 0, vertices->D3DResource.Get(), 0, vertexBytes);
    queue->ExecuteCommandList(commands);
    queue->Flush();
    void *first = nullptr, *second = nullptr;
    const D3D12_RANGE range = { 0, static_cast<SIZE_T>(vertexBytes) }, noWrite = {};
    ThrowIfFailed(positions[0]->Map(0, &range, &first));
    ThrowIfFailed(positions[1]->Map(0, &range, &second));
    Require(std::memcmp(first, second, static_cast<size_t>(vertexBytes)) == 0,
        "Depth and Equal GBuffer expansions changed the frozen frame positions");
    positions[0]->Unmap(0, &noWrite);
    positions[1]->Unmap(0, &noWrite);
    for (UINT slot = 0; slot < resources.FrameConstants.size(); ++slot)
    {
        resources.CurrFrameConstantsIndex = slot;
        renderer.BeginFrame(frozenCommon, {grass}, {wind}, controls);
        auto firstView = GrassTestView(resources, GDX12GrassRenderer::Stage::Depth, vp, eye,
            resources.FrameConstants[slot]->CameraCB->GetElementAddress(0));
        firstView.Key = 7200;
        auto lastView = firstView;
        for (UINT i = 0; i <= 64; ++i)
        {
            auto view = firstView;
            view.Key += i;
            if (i == 64) view.ViewProjection = Matrix::CreateTranslation(5, 0, 0) * vp;
            renderer.PrepareView(view, 0);
            lastView = view;
        }
        views.Views = { firstView, lastView };
        readA.Target = readbacks[slot][0].Get();
        readB.Target = readbacks[slot][1].Get();
        auto* draw = queue->GetCommandList();
        draw = graph.Execute(draw);
        queue->ExecuteCommandList(draw); queue->Flush();
        void *a = nullptr, *b = nullptr;
        ThrowIfFailed(readbacks[slot][0]->Map(0, nullptr, &a));
        ThrowIfFailed(readbacks[slot][1]->Map(0, nullptr, &b));
        bool different = false, coversA = false, coversB = false;
        for (UINT y = 0; y < footprint.Footprint.Height; ++y)
            for (UINT x = 0; x < footprint.Footprint.Width; ++x)
            {
                const auto offset = footprint.Offset + y * footprint.Footprint.RowPitch + x * sizeof(float);
                const float da = *reinterpret_cast<const float*>(static_cast<const BYTE*>(a) + offset);
                const float db = *reinterpret_cast<const float*>(static_cast<const BYTE*>(b) + offset);
                different |= da != db;
                coversA |= da > 0;
                coversB |= db > 0;
            }
        readbacks[slot][0]->Unmap(0, &noWrite);
        readbacks[slot][1]->Unmap(0, &noWrite);
        Require(different && coversA && coversB,
            "Prepared grass views across 64 constants reused the wrong projection or frame slot");
    }
    renderer.BeginFrame(frozenCommon, {}, {}, controls);
    renderer.PublishPrimitives(resources.Primitives, &source);
    Require(!resources.Primitives.Get(firstHandle), "Deleted grass owner retained its render primitive");
    GDX12GrassInstance capacity;
    capacity.Id = 7100;
    capacity.Implementation = GDX12GrassImplementation::CrossedRibbons;
    capacity.ExtentsCm = Vector2(55, 40);
    capacity.DensityPerSquareMetre = 1400;
    capacity.MaxBlades = 2048;
    capacity.Segments = 3;
    Require(GrassBladeCount(capacity) == 1232, "Default Bistro grass footprint lost its characterized blade count");
    capacity.CastShadows = false;
    renderer.BeginFrame(frozenCommon, {capacity}, {}, controls);
    renderer.PublishPrimitives(resources.Primitives, &source);
    const auto shadowPatches = renderer.FreezeShadowPatches(resources.Primitives, source);
    const auto reflectionPatches = renderer.FreezeShadowPatches(resources.Primitives, source, false);
    Require(shadowPatches && shadowPatches->empty() && reflectionPatches && reflectionPatches->size() == 1,
        "Reflection capture omitted visible grass that does not cast shadows");
    capacity.CastShadows = true;
    const auto defaultBytes = renderer.GeometryAllocationBytes();
    std::vector<GDX12GrassInstance> patches(16, capacity);
    for (UINT i = 0; i < patches.size(); ++i) patches[i].Id += i;
    renderer.BeginFrame(frozenCommon, patches, {}, controls);
    Require(renderer.PatchCount() == 16 && renderer.GeometryAllocationBytes() == defaultBytes,
        "Grass arena allocation grew with patch count instead of the largest patch");
    for (auto& patch : patches)
    {
        patch.ExtentsCm = Vector2(100000, 100000);
        patch.DensityPerSquareMetre = 1;
        patch.MaxBlades = 262144;
        patch.Segments = 8;
    }
    renderer.BeginFrame(frozenCommon, patches, {}, controls);
    const auto maximumBytes = renderer.GeometryAllocationBytes();
    Require(renderer.PatchCount() == 16 && maximumBytes < 493ull * 1024 * 1024,
        "Maximum valid grass settings multiplied arena capacity by patches, views or frame slots");
    std::cout << "Grass physical geometry arena: default1patch=" << defaultBytes << " default16patches=" << defaultBytes
        << " max16patches=" << maximumBytes << " bytes; one arena across all direct-queue views and frame slots.\n";
    resources.GeometrySources.pop_back();
    resources.CurrFrameConstantsIndex = savedSlot;
    std::cout << "Prepared grass: queued common batch consumers, two views, arena overwrite rejection, frozen depth/GBuffer and primitive wind revisions passed.\n";
}

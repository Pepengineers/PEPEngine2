#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12GeometryViewPass.h"
#include "Engine.RendererDX12/GDX12RenderGeometrySource.h"

class GDX12GeometryViewJobPass final : public GDX12RenderPass
{
public:
    GDX12GeometryViewJobPass(GDX12GeometryViewPass& pass, UINT slot) : _pass(pass), _slot(slot) {}
    void RecordRenderGraph(GDX12RenderGraphBuilder& graph) override
    { graph.SetName("Geometry view mesh batches"); graph.HasSideEffects(); }
    void Execute(GDX12CommandList* commands) override
    { if (_pass.GetGeometryView(_slot)) _pass.ExecuteGeometryView(commands, _slot); }
    std::vector<GDX12RenderGraph::BufferAccess> GetBufferAccesses() const override
    { return _pass.GetGeometryView(_slot) ? _pass.GetBufferAccesses() : std::vector<GDX12RenderGraph::BufferAccess>{}; }
private:
    GDX12GeometryViewPass& _pass;
    UINT _slot;
};

class GDX12GeometryBatchPass final : public GDX12RenderPass
{
public:
    GDX12GeometryBatchPass(GDX12DeviceResources& resources, GDX12GeometryViewPass& pass,
        UINT slot, const IRenderGeometrySource& source, UINT first, UINT count)
        : _deviceResources(resources), _pass(pass), _slot(slot), _source(source), _first(first), _count(count) {}
    void RecordRenderGraph(GDX12RenderGraphBuilder& graph) override
    { graph.SetName("Geometry source batches"); graph.HasSideEffects(); }
    void Execute(GDX12CommandList* commands) override
    {
        const auto* prepared = _pass.GetGeometryView(_slot);
        if (!prepared) return;
        auto view = *prepared;
        view.Source = &_source;
        view.BatchOffset = _first;
        view.BatchCount = _count;
        _pass.BindGeometryViewTargets(commands, _slot);
        _deviceResources.EmitGeometry(commands, view, {});
    }
    std::vector<GDX12RenderGraph::BufferAccess> GetBufferAccesses() const override
    {
        const auto* view = _pass.GetGeometryView(_slot);
        return view ? _source.GetGeometryBufferAccesses(view->Stage) : std::vector<GDX12RenderGraph::BufferAccess>{};
    }
private:
    GDX12DeviceResources& _deviceResources;
    GDX12GeometryViewPass& _pass;
    UINT _slot;
    const IRenderGeometrySource& _source;
    UINT _first, _count;
};

class GDX12GeometryFinalizePass final : public GDX12RenderPass
{
public:
    explicit GDX12GeometryFinalizePass(GDX12GeometryViewPass& pass) : _pass(pass) {}
    void RecordRenderGraph(GDX12RenderGraphBuilder& graph) override
    {
        // Publish the containing technique's outputs after its last range consumer.
        _pass.RecordRenderGraph(graph);
        graph.HasSideEffects();
    }
    void Execute(GDX12CommandList* commands) override { _pass.EndGeometry(commands); }
    std::vector<GDX12RenderGraph::BufferAccess> GetBufferAccesses() const override
    { return _pass.GetBufferAccesses(); }
private:
    GDX12GeometryViewPass& _pass;
};

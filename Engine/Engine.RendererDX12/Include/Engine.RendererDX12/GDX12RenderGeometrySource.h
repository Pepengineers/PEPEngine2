#pragma once

#include "Engine.RendererDX12/GDX12RenderGeometry.h"
class GDX12RenderGraphBuilder;

struct GDX12SceneContributions
{
    std::vector<GDX12LocalShadowCaster> Casters;
    uint64_t Revision = 14695981039346656037ull;
    bool HasBounds = false;
    BoundingBox Bounds;

    void Append(const BoundingBox& bounds, uint64_t revision, bool dynamic, bool castsShadows)
    {
        Revision = (Revision ^ revision) * 1099511628211ull;
        if (HasBounds) BoundingBox::CreateMerged(Bounds, Bounds, bounds);
        else Bounds = bounds;
        HasBounds = true;
        if (castsShadows) Casters.push_back({ bounds, revision, dynamic });
    }
};

// Geometry-only source; pipeline composition belongs to IRenderPipeline.
class IRenderGeometrySource
{
public:
    virtual ~IRenderGeometrySource() = default;
    virtual void GatherBatches(const GDX12GeometryContext&, std::vector<GDX12RenderBatch>&) const {}
    // Union of current prepared batch accesses for this stage; consumers query it before recording.
    virtual std::vector<GDX12RenderGraph::BufferAccess> GetGeometryBufferAccesses(GDX12GeometryStage) const { return {}; }
    virtual void AppendSceneContributions(GDX12SceneContributions&) const {}
    virtual void DeclareGeometryResources(GDX12RenderGraphBuilder&) const {}
    // shortcut: legacy grass expansion/draw bridge, remove when preparation publishes batches.
    virtual void DrawGeometry(GDX12CommandList*, const GDX12GeometryContext&) {}
};

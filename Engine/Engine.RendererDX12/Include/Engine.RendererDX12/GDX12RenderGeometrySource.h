#pragma once
#include "Engine.RendererDX12/GDX12RenderGeometry.h"

class IRenderGeometrySource
{
public:
    virtual ~IRenderGeometrySource() = default;
    virtual void GatherBatches(const GDX12GeometryContext&, std::vector<GDX12RenderBatch>&) const {}
    // Union of current prepared batch accesses for this stage; consumers query it before recording.
    virtual std::vector<GDX12RenderGraph::BufferAccess> GetGeometryBufferAccesses(GDX12GeometryStage) const { return {}; }
};

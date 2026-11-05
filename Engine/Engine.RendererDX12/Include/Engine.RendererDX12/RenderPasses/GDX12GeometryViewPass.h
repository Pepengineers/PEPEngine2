#pragma once
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include "Engine.RendererDX12/GDX12RenderGeometry.h"

// A technique selects views during CPU preparation; the pipeline can interleave
// application preparation and ordinary batch consumption within those views.
class GDX12GeometryViewPass : public GDX12RenderPass
{
public:
    virtual UINT GetGeometryViewCapacity() const = 0;
    virtual const GDX12GeometryContext* GetGeometryView(UINT slot) const = 0;
    virtual void BeginGeometry(GDX12CommandList*) = 0;
    virtual void ExecuteGeometryView(GDX12CommandList*, UINT slot) = 0;
    virtual void BindGeometryViewTargets(GDX12CommandList*, UINT slot) = 0;
    virtual void EndGeometry(GDX12CommandList*) = 0;
    void SetSplitGeometry(bool split) { _splitGeometry = split; }
    void Execute(GDX12CommandList* commands) override
    {
        BeginGeometry(commands);
        if (_splitGeometry) return;
        for (UINT slot = 0; slot < GetGeometryViewCapacity(); ++slot)
            if (GetGeometryView(slot)) ExecuteGeometryView(commands, slot);
        EndGeometry(commands);
    }

protected:
    bool _splitGeometry = false;
};

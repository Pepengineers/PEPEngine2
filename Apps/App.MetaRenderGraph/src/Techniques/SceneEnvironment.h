#pragma once
#include "Graph/MetaGraph.h"
#include "Engine.RendererDX12/RenderPasses/GDX12EnvironmentIBLPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12CloudComponent.h"
#include <functional>

namespace Meta
{
// Owner prepares every allocation, descriptor and constant before recording.
// Dispatch closures contain only immutable native bindings; the graph owns all
// transitions. Persistent outputs return to COMMON between frame submissions.
class SceneEnvironment
{
  public:
    SceneEnvironment(GDX12DeviceResources &, RenderPipelineCommonData &);
    ~SceneEnvironment();
    void Freeze(double simulationDelta);
    void SetClouds(const GDX12CloudParameters &);
    void ResetClouds();
    void Resize()
    {
        _environment.Resize();
    }
    void RestoreViewsAfterBaseline(); // diagnostic owner drain, never normal frame recording
    void Append(Graph &, std::vector<ID3D12Resource *> &);
    bool Record(Id, ID3D12GraphicsCommandList *) const;
    void Submitted(uint64_t fence);
    bool Ready() const;
    IRenderPassLink *Output(UINT index)
    {
        return _environment.GetOutput(index);
    }
    const GDX12CloudInputs &CloudInputs() const
    {
        return _cloudInput;
    }
    std::vector<Access> Reads(Graph &, std::vector<ID3D12Resource *> &) const;

  private:
    struct Work
    {
        std::string Name;
        std::vector<std::pair<GDX12Resource *, Access>> Accesses;
        std::function<void(ID3D12GraphicsCommandList *)> Record;
    };
    void Build();
    GDX12DeviceResources &_resources;
    RenderPipelineCommonData &_common;
    GDX12EnvironmentIBLPass _environment;
    GDX12CloudComponent _cloud;
    GDX12CloudInputs _cloudInput;
    std::vector<Work> _work;
    std::vector<Id> _operations;
    std::vector<std::pair<uint64_t, std::vector<Work>>> _retained;
    uint64_t _submitted = 0;
    bool _cloudSelected = false;
};
} // namespace Meta

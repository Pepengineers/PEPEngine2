#pragma once
#include "NativeWork.h"
#include "SceneEnvironment.h"
#include "SceneGeometry.h"
#include "Engine.RendererDX12/RenderPasses/GDX12ReflectionProbePass.h"
namespace BistroRender
{
class SceneReflections
{
  public:
    SceneReflections(GDX12DeviceResources&, RenderPipelineCommonData&);
    ~SceneReflections();
    void Freeze()
    {
        _settings = GetReflectionSettings();
        _historyBase = _common.HistoryRevision;
    }
    void Append(Graph&, std::vector<ID3D12Resource*>&, SceneGeometry&, SceneEnvironment&, UINT slot,
                GDX12Texture* localShadows = nullptr, GDX12Texture* staticLocalShadows = nullptr);
    bool Record(Id, ID3D12GraphicsCommandList*) const;
    void Submitted(uint64_t fence);
    void Reset();
    std::pair<size_t, size_t> Readiness() const;
    GDX12Texture* Published()
    {
        return &_passes->_published;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE Radiance() const
    {
        return _radiance.GPUHandle;
    }
    uint64_t History() const
    {
        return _history;
    }

  private:
    struct Publication
    {
        uint64_t Fence = 0;
        bool Ready = false;
    };
    GDX12DeviceResources& _resources;
    RenderPipelineCommonData& _common;
    std::unique_ptr<GDX12ReflectionProbePass> _passes;
    NativeWork _work;
    std::vector<UINT> _publishing;
    std::vector<Publication> _publications;
    GDX12Resource _black;
    GDX12Descriptor _radiance;
    uint64_t _history = 0;
    GDX12ReflectionSettings _settings;
    uint64_t _historyBase = 0;
};
} // namespace BistroRender

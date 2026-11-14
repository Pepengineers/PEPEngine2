#pragma once
#include "NativeWork.h"
#include "SceneEnvironment.h"
#include "SceneGeometry.h"
#include "Engine.RendererDX12/RenderPasses/GDX12ReflectionProbePass.h"
namespace Meta
{
class SceneReflections
{
  public:
    SceneReflections(GDX12DeviceResources &, RenderPipelineCommonData &);
    ~SceneReflections();
    void Freeze()
    {
        _settings = GetReflectionSettings();
        _historyBase = _common.HistoryRevision;
    }
    void Append(Graph &, std::vector<ID3D12Resource *> &, SceneGeometry &, SceneEnvironment &, UINT slot, bool serial);
    bool Record(Id, ID3D12GraphicsCommandList *) const;
    void Submitted(bool serial, uint64_t fence);
    void Reset();
    std::pair<size_t, size_t> Readiness() const;
    GDX12Texture *Published(bool serial)
    {
        return &_passes[serial ? 0 : 1]->_published;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE Radiance(bool serial) const
    {
        return _radiance[serial ? 0 : 1].GPUHandle;
    }
    uint64_t History(bool serial) const
    {
        return _history[serial ? 0 : 1];
    }
    std::vector<uint8_t> Readback(UINT slot, const std::string &stem) const;

  private:
    struct Image
    {
        UINT Subresource;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT Footprint;
        UINT Rows;
        UINT64 RowBytes;
    };
    struct FrameEvidence
    {
        GDX12ResourceHandle Buffer;
        std::vector<Image> Images;
        UINT64 Bytes = 0;
        UINT Captured = 0, Baked = 0, Published = 0;
    };
    struct Publication
    {
        uint64_t Fence = 0;
        bool Ready = false;
    };
    GDX12DeviceResources &_resources;
    RenderPipelineCommonData &_common;
    std::array<std::unique_ptr<GDX12ReflectionProbePass>, 2> _passes;
    std::array<NativeWork, 2> _work;
    std::array<std::vector<UINT>, 2> _publishing;
    std::array<std::vector<Publication>, 2> _publications;
    std::array<GDX12Resource, 2> _black;
    std::array<GDX12Descriptor, 2> _radiance;
    std::array<uint64_t, 2> _history{};
    std::array<std::vector<FrameEvidence>, 2> _evidence;
    UINT _mode = 1;
    GDX12ReflectionSettings _settings;
    uint64_t _historyBase = 0;
};
} // namespace Meta

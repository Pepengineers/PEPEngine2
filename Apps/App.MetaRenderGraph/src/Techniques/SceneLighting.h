#pragma once
#include "SceneAO.h"
#include "SceneReflections.h"
#include "Engine.RendererDX12/RenderPasses/GDX12DeferredLightingPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12SSSRPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12WBOITCompositionPass.h"
namespace Meta
{
class SceneLighting
{
  public:
    SceneLighting(GDX12DeviceResources &, RenderPipelineCommonData &);
    ~SceneLighting();
    void Freeze(SceneGeometry &, UINT slot);
    void AppendAO(Graph &, std::vector<ID3D12Resource *> &);
    void Append(Graph &, std::vector<ID3D12Resource *> &, SceneGeometry &, SceneEnvironment &, GDX12Texture *target,
                bool serial);
    bool Record(Id, ID3D12GraphicsCommandList *) const;
    void Submitted(bool serial, uint64_t fence);
    void Reset();
    std::pair<size_t, size_t> Readiness() const
    {
        return _pendingReset ? std::pair<size_t, size_t>{} : _reflections.Readiness();
    }
    std::vector<uint8_t> Readback(UINT slot, const std::string &stem) const;
    // Append consumers in this frame's graph. These pointers do not publish a
    // CPU-readable or cross-queue-ready result. Composition already includes
    // opaque aerial perspective; U6 should start with ReflectedHDR instead.
    GDX12Texture *LitHDR() const
    {
        return _deferred._color.get();
    }
    GDX12Texture *ReflectedHDR() const
    {
        return _ssr[_mode] ? &_ssr[_mode]->_color : nullptr;
    }
    GDX12Texture *ComposedHDR() const
    {
        return _composition.OUT_Result.get();
    }

  private:
    struct Frame
    {
        std::unique_ptr<SceneAO> AO;
        GDX12ResourceHandle Readback;
        std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, 4> Footprints;
        UINT64 Bytes = 0;
        UINT Width = 0, Height = 0;
    };
    GDX12DeviceResources &_resources;
    RenderPipelineCommonData &_common;
    SceneReflections _reflections;
    std::vector<Frame> _frames;
    GDX12DeferredLightingPass _deferred;
    GDX12WBOITCompositionPass _composition;
    std::array<std::unique_ptr<GDX12SSSRPass>, 2> _ssr;
    std::array<NativeWork, 2> _work;
    GDX12Texture _neutralAccum, _neutralReveal;
    std::unique_ptr<GDX12RootSignature> _displayRoot;
    ComPtr<ID3D12PipelineState> _displayPipeline;
    std::array<bool, 2> _forceReset{true, true};
    UINT _slot = 0, _mode = 1;
    bool _initialized = false;
    bool _pendingReset = false;
    GDX12SSRSettings _ssrSettings{};
    GDX12LightGridSettings _gridSettings{};
    UINT _aoTemporalIndex = 0;
};
} // namespace Meta

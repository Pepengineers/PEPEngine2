#pragma once
#include "SceneAO.h"
#include "SceneReflections.h"
#include "SceneSunShadows.h"
#include "SceneLocalShadows.h"
#include "SceneTransparency.h"
#include "SceneProbePreview.h"
#include "Engine.RendererDX12/RenderPasses/GDX12DeferredLightingPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12SSSRPass.h"
namespace Meta
{
class SceneLighting
{
  public:
    SceneLighting(GDX12DeviceResources &, RenderPipelineCommonData &);
    ~SceneLighting();
    void Freeze(SceneGeometry &, UINT slot);
    void AppendAO(Graph &, std::vector<ID3D12Resource *> &);
    void Append(Graph &, std::vector<ID3D12Resource *> &, SceneGeometry &, SceneEnvironment &, GDX12Texture *target);
    using HDRContinuation = std::function<GDX12Texture*(Graph&, std::vector<ID3D12Resource*>&,
        GDX12Texture*, GDX12Texture*)>;
    void SetHDRContinuation(HDRContinuation continuation) { _hdrContinuation = std::move(continuation); }
    bool Record(Id, ID3D12GraphicsCommandList *) const;
    void Submitted(uint64_t fence);
    D3D12_GPU_DESCRIPTOR_HANDLE ProbeRadiance() const { return _reflections->Radiance(); }
    void Reset();
    GDX12Texture* ParticleMotion() const { return _transparency ? _transparency->ParticleMotion() : nullptr; }
    GDX12Texture* PostDepth() const { return _transparency ? _transparency->Depth() : nullptr; }
    GDX12Texture* PostMotion() const { return _transparency ? _transparency->Motion() : nullptr; }
    void SetWaterRetirement(std::function<void()> retire) { if (_transparency) _transparency->SetWaterRetirement(std::move(retire)); }
    std::pair<size_t, size_t> Readiness() const
    {
        return _pendingReset || !_reflections ? std::pair<size_t, size_t>{} : _reflections->Readiness();
    }
    GDX12Texture *LitHDR() const
    {
        return _deferred._color.get();
    }
    GDX12Texture *ReflectedHDR() const
    {
        return _ssr ? &_ssr->_color : nullptr;
    }

  private:
    struct Frame
    {
        std::unique_ptr<SceneAO> AO;
        UINT Width = 0, Height = 0;
        uint64_t LightRevision = 0;
        GDX12ResourceHandle LightSource;
        bool UploadLights = false;
    };
    void AppendGpuLightGrid(Graph&, std::vector<ID3D12Resource*>&, const SceneGeometry&);
    HDRContinuation _hdrContinuation;
    GDX12DeviceResources &_resources;
    RenderPipelineCommonData &_common;
    std::unique_ptr<SceneReflections> _reflections;
    std::unique_ptr<SceneProbePreview> _probePreview;
    std::unique_ptr<SceneLocalShadows> _probeStaticShadows;
    std::unique_ptr<SceneSunShadows> _sunShadows;
    std::unique_ptr<SceneLocalShadows> _localShadows;
    std::unique_ptr<SceneTransparency> _transparency;

    std::vector<Frame> _frames;
    GDX12DeferredLightingPass _deferred;
    std::unique_ptr<GDX12SSSRPass> _ssr;
    NativeWork _work;
    std::unique_ptr<GDX12RootSignature> _gridRoot;
    ComPtr<ID3D12PipelineState> _gridBuild;
    bool _forceReset = true;
    UINT _slot = 0;
    bool _initialized = false;
    bool _pendingReset = false;
    GDX12SSRSettings _ssrSettings{};
    GDX12LightGridSettings _gridSettings{};
    UINT _aoTemporalIndex = 0;
};
} // namespace Meta

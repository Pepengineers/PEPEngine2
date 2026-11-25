#pragma once
#include "Engine.RendererDX12/GDX12InstanceBatcher.h"
#include "Engine.RendererDX12/GDX12RenderGeometrySource.h"
#include <cstdint>
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include <array>
#include <functional>
#include <optional>
#include <utility>
namespace BistroRender { class SceneReflections; }

class GDX12ReflectionProbePass final : public GDX12RenderPass
{
public:
    GDX12ReflectionProbePass();
    void Initialize() override;
    void Prepare();
    void ResetHistory();
    void ClearDenendencies() override;

    struct Statistics
    {
        UINT CapturedFaces = 0;
        UINT BakedFaces = 0;
        UINT PublishedProbes = 0;
        uint64_t TextureBytes = 0;
    };
    const Statistics& GetStatistics() const { return _statistics; }
    std::pair<size_t, size_t> GetReadiness();

private:
    friend class BistroRender::SceneReflections;
    enum Geometry : std::uint8_t { AllGeometry, StaticGeometry, DynamicGeometry };

    struct ProbeState
    {
        GDX12ReflectionProbe Source;
        GDX12ReflectionProbe Published;
        UINT BakedFaces = 0;
        uint64_t LastUpdate = 0;
        bool Ready = false;
        bool OwnsCapture = false;
        UINT CapturedFaces = 0, RawFaces = 0, NextFace = 0;
        bool RebakePending = false;
        std::unique_ptr<GDX12Texture> Raw;
        std::vector<std::unique_ptr<GDX12Descriptor>> RawRTVs, RawMipSRVs;
        std::unique_ptr<GDX12Texture> BakedColor, BakedDepth;
        std::vector<std::unique_ptr<GDX12Descriptor>> BakedRTVs, BakedDSVs;
    };
    struct FrameData
    {
        std::unique_ptr<GDX12UploadBuffer<GDX12CameraConstants>> Cameras;
        std::unique_ptr<GDX12UploadBuffer<GDX12IndirectDrawArgs>> Commands;
        std::unique_ptr<GDX12UploadBuffer<UINT>> Counter;
        std::unique_ptr<GDX12InstanceBatcher> Batches;
    };
    struct PreparedView
    {
        GDX12GeometryContext Context;
        GDX12CaptureLighting Lighting;
        Geometry Filter = AllGeometry;
        UINT Face = 0;
        UINT Probe = 0;
        bool Bake = false;
        bool Selected = false;
    };

    void CreatePublishedTexture(UINT count, UINT resolution);
    void CreateCaptureTextures(UINT resolution);
    void SelectProbe(UINT probe);
    void CreateBakedTextures(ProbeState& probe);
    void CreateTexture(GDX12Texture& texture, UINT size, UINT slices, UINT mips, D3D12_RESOURCE_FLAGS flags, bool depth);
    void CreateFaceViews(GDX12Texture& texture, std::vector<std::unique_ptr<GDX12Descriptor>>& views, bool depth);

    std::vector<ProbeState> _probes;
    std::vector<FrameData> _frames;
    std::unique_ptr<GDX12RootSignature> _captureRoot, _cullRoot, _filterRoot;
    ComPtr<ID3D12PipelineState> _capturePSO, _skyPSO, _cullPSO, _clearPSO, _filterPSO, _downsamplePSO;
    ComPtr<ID3D12CommandSignature> _signature;
    const GDX12ReflectionSettings* _externalSettings = nullptr;
    std::function<bool(UINT)> _ownsCapture;
    std::function<bool(UINT)> _captureAllowed;
    std::function<UINT(UINT)> _sceneFaces;
    GDX12Texture _published;
    struct CaptureTextures { GDX12Texture Filtered, Depth; };
    std::optional<CaptureTextures> _capture;
    std::vector<std::unique_ptr<GDX12Descriptor>> _depthDSVs, _filterUAVs;
    Statistics _statistics;
    GDX12ReflectionSettings _settings;
    uint64_t _staticRevision = 0;
    uint64_t _staticGeometryRevision = 0;
    uint64_t _environmentRevision = 0;
    uint64_t _frameNumber = 0;
    UINT _activeProbe = 0;
    UINT _roundRobinProbe = 0;
    UINT _resolution = 0;
    UINT _mips = 0;
    UINT _captureTextureResolution = 0;
    UINT _publishedResolution = 0;
    UINT _publishedMips = 0;
    bool _enabled = false;
    std::vector<PreparedView> _views;
    GDX12ReflectionSchedule::Scheduler _scheduler;
    bool _planPrepared = false;
    bool _capturePlanned = false;
    bool _publishedNeedsTransition = false;
};

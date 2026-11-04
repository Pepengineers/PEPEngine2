#pragma once
#include <cstdint>
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12LocalShadowPass.h"
#include <array>

class GDX12ReflectionProbePass final : public GDX12RenderPass
{
public:
    void RecordRenderGraph(GDX12RenderGraphBuilder& graph) override
    {
        graph.SetName("Reflection probes");
        graph.Read(SharedResource::Scene);
        graph.ReadInputs(_inputs);
        graph.Read(SharedResource::Environment);
        graph.Write(SharedResource::ReflectionProbes, "Reflection probe cache");
        graph.HasSideEffects();
    }

    GDX12ReflectionProbePass();
    void Initialize() override;
    void Prepare();
    void Execute(GDX12CommandList* commands) override;
    void ClearDenendencies() override;

    struct Statistics
    {
        UINT CapturedFaces = 0;
        UINT BakedFaces = 0;
        UINT PublishedProbes = 0;
        uint64_t TextureBytes = 0;
    };
    const Statistics& GetStatistics() const { return _statistics; }

private:
    enum Geometry : std::uint8_t { AllGeometry, StaticGeometry, DynamicGeometry };

    struct ProbeState
    {
        GDX12ReflectionProbe Source;
        GDX12ReflectionProbe Published;
        UINT BakedFaces = 0;
        uint64_t LastUpdate = 0;
        bool Ready = false;
        std::unique_ptr<GDX12Texture> BakedColor, BakedDepth;
        std::vector<std::unique_ptr<GDX12Descriptor>> BakedRTVs, BakedDSVs;
    };
    struct FrameData
    {
        std::unique_ptr<GDX12UploadBuffer<GDX12CameraConstants>> Cameras;
        std::unique_ptr<GDX12UploadBuffer<GDX12ShadowConstants>> Shadow;
        std::unique_ptr<GDX12UploadBuffer<GDX12IndirectDrawArgs>> Commands;
        std::unique_ptr<GDX12UploadBuffer<UINT>> Counter;
    };

    void CreatePublishedTexture(GDX12CommandList* commands, UINT count, UINT resolution);
    void CreateCaptureTextures(UINT resolution);
    void CreateBakedTextures(ProbeState& probe);
    void CreateTexture(GDX12Texture& texture, UINT size, UINT slices, UINT mips, D3D12_RESOURCE_FLAGS flags, bool depth);
    void CreateFaceViews(GDX12Texture& texture, std::vector<std::unique_ptr<GDX12Descriptor>>& views, bool depth);
    void Cull(GDX12CommandList* commands, UINT camera, Geometry geometry, bool shadow);
    void BindCapture(GDX12CommandList* commands, UINT face, GDX12Texture* localShadows);
    void RenderShadow(GDX12CommandList* commands, Geometry geometry);
    void RenderFace(GDX12CommandList* commands, UINT probe, UINT face, Geometry geometry, bool bake);
    void FilterAndPublish(GDX12CommandList* commands, UINT probe);
    void WriteProbeData(GDX12CommandList* commands, bool enabled);

    std::vector<ProbeState> _probes;
    std::vector<FrameData> _frames;
    std::unique_ptr<GDX12RootSignature> _captureRoot, _cullRoot, _filterRoot;
    ComPtr<ID3D12PipelineState> _capturePSO, _skyPSO, _shadowPSO, _cullPSO, _clearPSO, _filterPSO;
    ComPtr<ID3D12CommandSignature> _signature;
    std::unique_ptr<GDX12LocalShadowPass> _staticLocalShadows;
    GDX12Texture _published, _raw, _filtered, _depth, _sunShadow;
    bool _sunShadowNeedsClear = true;
    std::vector<std::unique_ptr<GDX12Descriptor>> _rawRTVs, _depthDSVs, _sunDSV, _filterUAVs, _rawMipSRVs;
    Statistics _statistics;
    GDX12ReflectionSettings _settings;
    uint64_t _staticRevision = 0;
    uint64_t _environmentRevision = 0;
    uint64_t _frameNumber = 0;
    UINT _activeProbe = 0;
    UINT _face = 0;
    UINT _resolution = 0;
    UINT _mips = 0;
    UINT _captureTextureResolution = 0;
    UINT _publishedResolution = 0;
    UINT _publishedMips = 0;
    bool _enabled = false;
};

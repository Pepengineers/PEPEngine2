#pragma once
#include "Engine.RendererDX12/GDX12InstanceBatcher.h"
#include "Engine.RendererDX12/GDX12RenderGeometrySource.h"
#include <cstdint>
#include "Engine.RendererDX12/RenderPasses/GDX12GeometryViewPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12LocalShadowPass.h"
#include <array>
#include <utility>

class GDX12ReflectionProbePass final : public GDX12GeometryViewPass
{
public:
    std::vector<GDX12RenderGraph::BufferAccess> GetBufferAccesses() const override
    {
        if (!_resources) return {};
        auto accesses = _resources->GetGeometryBufferAccesses(GDX12GeometryStage::ReflectionCapture);
        const auto shadows = _resources->GetGeometryBufferAccesses(GDX12GeometryStage::Shadow);
        accesses.insert(accesses.end(), shadows.begin(), shadows.end());
        return accesses;
    }

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
    void ResetHistory();
    void OnSubmitted(GDX12Submission::CompletionPoint point);
    void EnableSecondaryCapture(bool enabled) { _secondaryCapture = enabled; }
    const std::vector<GDX12ReflectionSchedule::Work>& ScheduledWork() const { return _scheduledWork; }
    double FrameSourceTime() const { return _frameSourceTime; }
    void SetRemoteWork(std::vector<GDX12ReflectionSchedule::Work> work, double sourceTime)
    { _remoteWork = std::move(work); _remoteSourceTime = sourceTime; }
    struct FacePayload { GDX12Texture* Texture; UINT Subresource; double SourceTime; };
    FacePayload RawFace(UINT probe, UINT face);
    GDX12Texture& PublishedTexture() { return _published; }
    GDX12Texture* CaptureLocalShadows() { return _inputs[4]->GetTexture(); }
    UINT PublishedMips() const { return _publishedMips; }
    bool PublishedThisFrame(UINT probe) const { return _probes.at(probe).LastUpdate == _frameNumber; }
    const std::array<double, 6>& PublishedSourceTimes(UINT probe) const { return _probes.at(probe).PublishedTimes; }
    void ImportFace(UINT probe, UINT face, double sourceTime);
    void ImportPublished(UINT probe, const std::array<double, 6>& sourceTimes);
    bool CanImportPublished(UINT probe, const std::array<double, 6>& times) const
    { return _probes.at(probe).CapturedFaces == 63 && _probes.at(probe).SourceTimes == times; }
    UINT GetGeometryViewCapacity() const override { return static_cast<UINT>(_views.size()); }
    const GDX12GeometryContext* GetGeometryView(UINT slot) const override;
    void BeginGeometry(GDX12CommandList* commands) override;
    void ExecuteGeometryView(GDX12CommandList* commands, UINT slot) override;
    void BindGeometryViewTargets(GDX12CommandList* commands, UINT slot) override;
    void EndGeometry(GDX12CommandList* commands) override;
    void ClearDenendencies() override;

    struct Statistics
    {
        UINT CapturedFaces = 0;
        UINT BakedFaces = 0;
        UINT PublishedProbes = 0;
        uint64_t TextureBytes = 0;
        double MaxFaceAgeSeconds = 0;
        double AdditionalLagSeconds = 0;
        UINT RequestedSecondaryFaces = 0, EffectiveSecondaryFaces = 0;
    };
    const Statistics& GetStatistics() { if (_resources) PollPublications(); return _statistics; }
    std::pair<size_t, size_t> GetReadiness();
    struct FaceAge { uint64_t Probe; UINT Face; double Seconds; };
    std::vector<FaceAge> GetFaceAges() const;

private:
    enum Geometry : std::uint8_t { AllGeometry, StaticGeometry, DynamicGeometry };

    struct ProbeState
    {
        GDX12ReflectionProbe Source;
        GDX12ReflectionProbe Published;
        UINT BakedFaces = 0;
        uint64_t LastUpdate = 0;
        bool Ready = false;
        UINT CapturedFaces = 0, NextFace = 0;
        bool RebakePending = false, PublicationRecorded = false;
        uint64_t Epoch = 0;
        std::array<double, 6> SourceTimes{}, PublishedTimes{};
        GDX12ReflectionSchedule::PublicationClock Clock;
        std::unique_ptr<GDX12Texture> Raw;
        std::vector<std::unique_ptr<GDX12Descriptor>> RawRTVs, RawMipSRVs;
        std::unique_ptr<GDX12Texture> BakedColor, BakedDepth;
        std::vector<std::unique_ptr<GDX12Descriptor>> BakedRTVs, BakedDSVs;
    };
    struct FrameData
    {
        std::unique_ptr<GDX12UploadBuffer<GDX12CameraConstants>> Cameras;
        std::unique_ptr<GDX12UploadBuffer<GDX12ShadowConstants>> Shadow;
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

    void PollPublications();
    void UpdateAgeStatistics();
    void CreatePublishedTexture(UINT count, UINT resolution);
    void CreateCaptureTextures(UINT resolution);
    void SelectProbe(UINT probe);
    void CreateBakedTextures(ProbeState& probe);
    void CreateTexture(GDX12Texture& texture, UINT size, UINT slices, UINT mips, D3D12_RESOURCE_FLAGS flags, bool depth);
    void CreateFaceViews(GDX12Texture& texture, std::vector<std::unique_ptr<GDX12Descriptor>>& views, bool depth);
    void Cull(GDX12CommandList* commands, UINT camera, Geometry geometry, bool shadow);
    GDX12GraphicsBindings CaptureBindings(UINT face, GDX12Texture* localShadows);
    void RenderShadow(GDX12CommandList* commands, UINT slot);
    void RenderFace(GDX12CommandList* commands, UINT slot);
    void FilterAndPublish(GDX12CommandList* commands, UINT probe);
    void WriteProbeData(GDX12CommandList* commands, bool enabled);

    struct PendingPublication
    {
        UINT Probe;
        uint64_t Epoch;
        std::array<double, 6> Sources;
        GDX12Submission::CompletionPoint Completion;
    };
    std::deque<PendingPublication> _publications;
    std::vector<ProbeState> _probes;
    std::vector<FrameData> _frames;
    std::unique_ptr<GDX12RootSignature> _captureRoot, _cullRoot, _filterRoot;
    ComPtr<ID3D12PipelineState> _capturePSO, _skyPSO, _shadowPSO, _cullPSO, _clearPSO, _filterPSO;
    ComPtr<ID3D12CommandSignature> _signature;
    std::unique_ptr<GDX12LocalShadowPass> _staticLocalShadows;
    GDX12Texture _published, _filtered, _depth, _sunShadow;
    bool _sunShadowNeedsClear = true;
    std::vector<std::unique_ptr<GDX12Descriptor>> _depthDSVs, _sunDSV, _filterUAVs;
    Statistics _statistics;
    GDX12ReflectionSettings _settings;
    GDX12ShadowQuality _shadowQuality;
    uint64_t _staticRevision = 0;
    uint64_t _environmentRevision = 0;
    uint64_t _frameNumber = 0;
    UINT _activeProbe = 0;
    UINT _roundRobinProbe = 0;
    UINT _resolution = 0;
    UINT _mips = 0;
    UINT _captureTextureResolution = 0;
    UINT _publishedResolution = 0;
    UINT _publishedMips = 0;
    double _frameSourceTime = 0;
    bool _enabled = false;
    std::vector<PreparedView> _views;
    GDX12ReflectionSchedule::Scheduler _scheduler;
    bool _planPrepared = false;
    bool _capturePlanned = false;
    bool _staticShadowsPlanned = false;
    bool _publishedNeedsTransition = false;
    bool _secondaryCapture = false;
    double _remoteSourceTime = 0;
    std::optional<std::vector<GDX12ReflectionSchedule::Work>> _remoteWork;
    std::vector<GDX12ReflectionSchedule::Work> _scheduledWork;
};

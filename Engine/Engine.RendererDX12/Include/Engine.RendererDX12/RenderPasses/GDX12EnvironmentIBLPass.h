#pragma once
#include "Engine.RendererDX12/GDX12UploadBuffer.h"
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include "Engine.Core/AssetManager.h"
#include "Engine.RendererDX12/RenderPasses/GDX12CloudComponent.h"
#include <array>
#include <chrono>
#include <filesystem>
#include <climits>
#include <optional>

struct GDX12CloudPublicationSettings
{
    double UpdateHz = 0;
    double MaxAgeMs = 0;
};
GDX12CloudPublicationSettings GetCloudPublicationSettings();

struct GDX12CloudSnapshotAge
{
    double Milliseconds = 0;
    bool OverAge = false;
};

struct GDX12CloudEnvironmentInputs
{
    using Clock = std::chrono::steady_clock;
    GDX12CloudInputs Cloud;
    SkyIBLSettings IBL;
    Vector3 SunDirection = Vector3(0.0f);
    Vector3 SunRadiance = Vector3(0.0f);
    uint64_t SunRevision = 0;
    uint64_t SkyRevision = 0;
    uint64_t EnvironmentRevision = 0;
    Clock::time_point CapturedAt;

    bool IsCompatibleWith(const GDX12CloudEnvironmentInputs& other) const;
    GDX12CloudSnapshotAge MeasureAge(Clock::time_point displayedAt, double maxAgeMs) const;
};

// A cap on producer attempts in host time. Zero is uncapped.
class GDX12CloudCadence
{
public:
    bool TakeTick(double nowSeconds, double maxHz);

private:
    double _rate = 0;
    double _origin = 0;
    double _lastTick = -1;
};

// One D3D12 buffer carries all faces and mips of irradiance, GGX and the source cube.
class GDX12CloudEnvironmentLayout
{
public:
    GDX12CloudEnvironmentLayout(ID3D12Device& device, UINT cubeSize);
    UINT CubeSize() const { return _cubeSize; }
    D3D12_RESOURCE_DESC BufferDesc() const { return CD3DX12_RESOURCE_DESC::Buffer(_bytes); }
    // All textures are already in COPY_DEST and the payload is in COPY_SOURCE.
    void RecordUnpack(GDX12CommandList* commands, const std::array<ID3D12Resource*, 3>& outputs,
        ID3D12Resource& payload) const;

private:
    friend class GDX12EnvironmentIBLPass;
    void Validate(ID3D12Resource& payload) const;
    void Copy(ID3D12GraphicsCommandList& commands, size_t cube, ID3D12Resource& texture,
        ID3D12Resource& payload, bool pack) const;

    UINT _cubeSize;
    UINT64 _bytes = 0;
    std::array<std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT>, 3> _footprints;
};

// Outputs: diffuse irradiance, GGX radiance (8 mips when enabled), DFG, source cube.
class GDX12EnvironmentIBLPass final : public GDX12RenderPass
{
public:
    class CloudProducer;

    void RecordRenderGraph(GDX12RenderGraphBuilder& graph) override
    {
        graph.SetName("Environment");
        graph.Read(SharedResource::Scene);
        graph.ReadInputs(_inputs);
        for (UINT i = 0; i < _outputs.size(); ++i)
            graph.Write(_outputs[i], "Environment " + std::to_string(i), Lifetime::Dynamic);
        graph.Write(SharedResource::Environment, "Atmosphere cache");
        graph.HasSideEffects();
    }

    GDX12EnvironmentIBLPass();
    void Initialize() override;
    void Resize() override;
    void Execute(GDX12CommandList* commands) override;
    void Invalidate();
    void ClearDenendencies() override;
    void SetCloudSource(GDX12CloudComponent* source);
    std::optional<GDX12CloudEnvironmentInputs> CaptureCloudInputs();
    // Exact/Latest replacement uses the stable display textures after the first local fallback.
    bool IsCloudDisplayPrepared(const GDX12CloudEnvironmentLayout& layout) const;
    void PublishCloudSnapshot(const GDX12CloudEnvironmentInputs& input);
    void RepeatCloudSnapshot(GDX12CommandList* commands, const GDX12CloudEnvironmentInputs& input);
private:
    void InitializeKernels(bool cloudProducer);
    void LoadEnvironment(const std::filesystem::path& asset);
    void CreateTexture(GDX12Texture& texture, UINT width, UINT height, UINT depth, UINT mips, bool cube);
    void EnsureOutputs(bool enabled, UINT cubeSize);
    void AllocateOutputs(bool enabled, UINT cubeSize);
    void EnsureAtmosphere(const SkyAtmosphereSettings& settings);
    void PublishAtmosphereViews(bool enabled);
    void BindSky(GDX12CommandList* commands, UINT constantIndex);
    void BindSkyConstants(GDX12CommandList* commands, D3D12_GPU_VIRTUAL_ADDRESS address);
    void DispatchLUT(GDX12CommandList* commands, UINT pipeline, UINT index);
    void RecordSourceCube(GDX12CommandList* commands);
    void RecordDisabledOutputs(GDX12CommandList* commands);
    void Filter(GDX12CommandList* commands, UINT samples, float intensity, bool display);
    void EnsureBRDF(GDX12CommandList* commands);
    void PackCloudPayload(GDX12CommandList* commands, const GDX12CloudEnvironmentLayout& layout,
        ID3D12Resource& payload);
    void UpdateCloudSkyConstants();
    void ResetEnvironmentHistory();
    void ReleaseAtmosphere();
    std::array<std::unique_ptr<GDX12Texture>, 4> _textures;
    std::array<std::unique_ptr<GDX12Texture>, 5> _luts;
    std::unique_ptr<GDX12Texture> _boxTexture;
    std::unique_ptr<GDX12UploadBuffer<std::array<uint16_t, 4>>> _disabledPixel;
    std::array<std::unique_ptr<GDX12Descriptor>, 8> _prefilterUAVs;
    std::array<ComPtr<ID3D12PipelineState>, 3> _filters;
    std::array<ComPtr<ID3D12PipelineState>, 5> _skyPipelines;
    std::unique_ptr<GDX12RootSignature> _filterRoot, _skyRoot;
    std::filesystem::path _boxAsset;
    GDX12Descriptor _nullCube, _null2D;
    std::vector<std::unique_ptr<GDX12Descriptor>> _sourceUAVs;
    UINT _prefilterMips = 0;
    uint64_t _revision = UINT64_MAX;
    UINT _quality = UINT_MAX;
    SkySource _source = SkySource::Off;
    bool _invalidated = true;
    bool _brdfReady = false;
    GDX12CloudComponent* _cloudSource = nullptr;
    uint64_t _cloudSourceRevision = 0, _cloudLightingRevision = 0;
    uint64_t _inputRevision = 1;
    bool _cloudActive = false;
    SkyIBLSettings _resolvedIBL;
    std::optional<GDX12CloudEnvironmentInputs> _displayedCloud;
};

// One retired invocation slot. The caller owns admission, submission and retirement.
class GDX12EnvironmentIBLPass::CloudProducer
{
public:
    CloudProducer(GDX12DeviceResources& resources, const GDX12CloudEnvironmentLayout& layout);
    // Payload enters and leaves in COPY_DEST. All private scratch returns to COMMON.
    void Record(GDX12CommandList* commands, const GDX12CloudEnvironmentInputs& input, ID3D12Resource& payload);

private:
    GDX12CloudEnvironmentLayout _layout;
    GDX12CloudComponent _cloud;
    GDX12EnvironmentIBLPass _environment;
    GDX12UploadBuffer<GDX12SkyConstants> _constants;
};

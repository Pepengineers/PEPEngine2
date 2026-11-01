#pragma once
#include "Engine.RendererDX12/GDX12GrassWind.h"
#include "Engine.RendererDX12/GDX12UploadBuffer.h"
#include "Engine.RendererDX12/GDX12RootSignature.h"
#include <array>
#include <unordered_map>

class GDX12CommandList;
class GDX12DeviceResources;
class RenderPipelineCommonData;
class GDX12Texture;

// Owned by the renderer, never a singleton. All calls use its direct queue.
class GDX12GrassRenderer
{
public:
    enum class Stage : UINT { Full, GBuffer, PrepassNormals, Depth, Shadow, Capture };
    struct CaptureLighting
    {
        GDX12Texture* SunShadow = nullptr;
        GDX12Texture* Irradiance = nullptr;
        GDX12Texture* Prefiltered = nullptr;
        GDX12Texture* BRDF = nullptr;
        GDX12Texture* LocalShadows = nullptr;
        D3D12_GPU_VIRTUAL_ADDRESS ShadowConstants = 0;
    };
    explicit GDX12GrassRenderer(GDX12DeviceResources* resources);
    ~GDX12GrassRenderer();
    void BeginFrame(GDX12CommandList* commands, const RenderPipelineCommonData& common,
        std::vector<GDX12GrassInstance> grass, std::vector<GDX12WindInstance> wind, GDX12GrassWindControls controls);
    void Draw(GDX12CommandList* commands, Stage stage, const Matrix& viewProjection,
        Vector3 eye, D3D12_GPU_VIRTUAL_ADDRESS camera, const CaptureLighting* lighting = nullptr);
    bool HasCasters() const;
    bool GetBounds(BoundingBox& bounds) const;
    const GDX12GrassWindControls& Controls() const { return _controls; }
    uint64_t Revision() const { return _revision; }

private:
    struct Vertex
    {
        XMFLOAT3 Position, PreviousPosition, Normal;
        XMFLOAT2 UV;
    };
    static_assert(sizeof(Vertex) == 44);
    struct Parameters
    {
        XMFLOAT4X4 World, PreviousWorld, ViewProjection;
        XMFLOAT4 EyeMaxDistance;
        XMFLOAT4 PreviousEye;
        XMFLOAT4 Shape; // height, width, height variation, max bend
        XMFLOAT4 Lod; // near, far, bend seconds, unused
        XMFLOAT4 Patch; // extents xy, current time, previous time
        XMFLOAT4 WindArea; // center xy, extents xy
        XMFLOAT4 WindForce; // normalized direction xy, speed, gust amplitude
        XMFLOAT4 WindGust; // frequency, wavelength, maximum speed, unused
        UINT Count, Segments, Implementation, Seed;
        UINT PointSource, Material, WindMode, Shadow;
        UINT GridResolution, ResetHistory, Padding0, Padding1;
    };
    static_assert(sizeof(Parameters) == 368);
    struct WindParameters
    {
        XMFLOAT4 Area; // center xy, extents xy
        XMFLOAT4 Force; // direction xy, speed, gust amplitude
        XMFLOAT4 Gust; // frequency, wavelength, time, dt
        XMFLOAT4 Solver; // force, damping, max speed, unused
        UINT Resolution, Padding0, Padding1, Padding2;
    };
    static_assert(sizeof(WindParameters) == 80);
    struct GrassState
    {
        GDX12GrassInstance Source;
        Matrix PreviousWorld = Matrix::Identity;
        std::vector<std::unique_ptr<GDX12UploadBuffer<XMFLOAT3>>> Roots;
        bool ResetHistory = true;
        uint64_t SettingsHash = 0;
    };
    using FieldBuffer = GDX12UploadBuffer<XMFLOAT4>;
    struct WindState
    {
        GDX12WindInstance Source;
        std::array<std::unique_ptr<FieldBuffer>, 2> Velocity, Pressure;
        std::unique_ptr<FieldBuffer> Advected, Divergence;
        float Time = 0;
        float PreviousTime = 0;
        UINT Resolution = 0;
        bool Initialized = false;
        bool ResetHistory = true;
        uint64_t SettingsHash = 0;
    };
    struct Frame
    {
        std::vector<std::unique_ptr<GDX12UploadBuffer<Parameters>>> DrawParameters;
        std::vector<std::unique_ptr<GDX12UploadBuffer<WindParameters>>> WindParametersBuffers;
        // Descriptors and upload mappings must survive every referencing command list.
        std::vector<std::unique_ptr<FieldBuffer>> RetiredFields;
        std::vector<std::unique_ptr<GDX12UploadBuffer<Vertex>>> RetiredVertices;
        std::vector<std::unique_ptr<GDX12UploadBuffer<UINT>>> RetiredIndices;
        std::vector<GrassState> RetiredGrass;
        UINT DrawCursor = 0;
        UINT WindCursor = 0;
    };
    template<typename T> std::unique_ptr<GDX12UploadBuffer<T>> Buffer(UINT count, bool srv, bool uav);
    void Initialize();
    void UpdateWind(GDX12CommandList* commands, WindState& state, bool reset);
    void WindDispatch(GDX12CommandList* commands, UINT stage, const WindParameters& parameters,
        FieldBuffer* input, FieldBuffer* pressure, FieldBuffer* output);
    void RetireWind(WindState& state);
    Frame& CurrentFrame();

    GDX12DeviceResources* _resources;
    GDX12GrassWindControls _controls;
    std::vector<Frame> _frames;
    std::unordered_map<uint64_t, GrassState> _grass;
    std::unordered_map<uint64_t, WindState> _wind;
    std::vector<uint64_t> _drawOrder;
    std::unique_ptr<GDX12RootSignature> _computeRoot, _graphicsRoot, _windRoot;
    ComPtr<ID3D12PipelineState> _expandPSO, _clearPSO;
    std::array<ComPtr<ID3D12PipelineState>, 6> _graphicsPSO;
    std::array<ComPtr<ID3D12PipelineState>, 6> _windPSO;
    ComPtr<ID3D12CommandSignature> _signature;
    std::unique_ptr<GDX12UploadBuffer<Vertex>> _vertices;
    std::unique_ptr<GDX12UploadBuffer<UINT>> _indices, _arguments;
    std::unique_ptr<FieldBuffer> _neutralWind;
    uint64_t _revision = 0;
    Vector3 _previousEye = Vector3::Zero;
    Vector3 _eye = Vector3::Zero;
    bool _initialized = false;
};

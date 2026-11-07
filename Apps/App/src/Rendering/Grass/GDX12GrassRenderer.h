#pragma once
#include <cstdint>
#include "Rendering/Grass/GDX12GrassWind.h"
#include "Engine.RendererDX12/GDX12UploadBuffer.h"
#include "Engine.RendererDX12/GDX12RootSignature.h"
#include "Engine.RendererDX12/GDX12RenderGeometry.h"
#include <array>
#include "Components/BistroInteractionSource.h"
#include "Engine.RendererDX12/GDX12Particles.h"
#include <unordered_map>
#include <map>
#include <tuple>

class GDX12CommandList;
class GDX12DeviceResources;
class RenderPipelineCommonData;
class GDX12Texture;

// Owned by Bistro. Simulation may use COMPUTE; expansion and draw use DIRECT.
class GDX12GrassRenderer
{
public:
    enum class Stage : std::uint8_t { Full, GBuffer, PrepassNormals, Depth, Shadow, Capture };
    using CaptureLighting = GDX12CaptureLighting;
    explicit GDX12GrassRenderer(GDX12DeviceResources* resources);
    ~GDX12GrassRenderer();
    // Wait the current frame-slot fence before reusing staging or replacing its roots.
    void BeginFrame(const RenderPipelineCommonData& common,
        std::vector<GDX12GrassInstance> grass, std::vector<GDX12WindInstance> wind, GDX12GrassWindControls controls,
        const std::vector<BistroInteractionSource>& sources = {}, float deltaSeconds = 0);
    GDX12ParticleInteractionField InteractionField() const;
    void PrepareGPU(GDX12CommandList* commands);
    void PrepareView(const GDX12GeometryContext& view, UINT patch);
    bool Expand(GDX12CommandList* commands, const GDX12GeometryContext& view, UINT patch);
    void GatherBatches(const GDX12GeometryContext& view, std::vector<GDX12RenderBatch>& batches) const;
    std::vector<GDX12RenderGraph::BufferAccess> PreparationAccesses() const;
    std::vector<GDX12RenderGraph::BufferAccess> ExpansionAccesses(const GDX12GeometryContext& view, UINT patch) const;
    std::vector<GDX12RenderGraph::BufferAccess> DrawAccesses() const;
    UINT PatchCount() const { return static_cast<UINT>(_drawOrder.size()); }
    void PublishPrimitives(GDX12RenderPrimitiveRegistry&, const IRenderGeometrySource*);
    void RemovePrimitives(GDX12RenderPrimitiveRegistry&);
    uint64_t GeometryAllocationBytes() const;
    bool HasCasters() const;
    const GDX12GrassWindControls& Controls() const { return _controls; }

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
        XMFLOAT4 InteractionArea;
        UINT Count, Segments, Implementation, Seed;
        UINT PointSource, Material, WindMode, Shadow;
        UINT GridResolution, ResetHistory, Padding0, Padding1;
    };
    static_assert(sizeof(Parameters) == 384);
    struct WindParameters
    {
        XMFLOAT4 Area; // center xy, extents xy
        XMFLOAT4 Force; // direction xy, speed, gust amplitude
        XMFLOAT4 Gust; // frequency, wavelength, time, dt
        XMFLOAT4 Solver; // force, damping, max speed, unused
        UINT Resolution, Padding0, Padding1, Padding2;
    };
    static_assert(sizeof(WindParameters) == 80);
    struct InteractionParameters
    {
        XMFLOAT4 Area;
        float DeltaSeconds; UINT SourceCount, Reset, Resolution;
    };
    struct GrassState
    {
        GDX12GrassInstance Source;
        Matrix PreviousWorld = Matrix::Identity;
        std::vector<std::unique_ptr<GDX12UploadBuffer<XMFLOAT3>>> Roots, RootsUpload;
        std::vector<uint64_t> RootSignatures;
        bool ResetHistory = true;
        uint64_t SettingsHash = 0;
        GDX12RenderPrimitiveHandle Primitive;
        BoundingBox Bounds;
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
    struct WindJob
    {
        UINT Stage, Resolution;
        GDX12UploadBuffer<WindParameters>* Constants;
        FieldBuffer *Input, *Pressure, *Output;
    };
    struct Output
    {
        GDX12GeometryContext View;
        GDX12CaptureLighting Lighting;
        UINT Patch = 0;
        uint64_t Owner = 0;
        GDX12UploadBuffer<Parameters>* Constants = nullptr;
        UINT ConstantIndex = 0;
        FieldBuffer *CurrentWind = nullptr, *PreviousWind = nullptr;
        GDX12GraphicsBindings Binding;
        GDX12RenderBatch Batch;
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
        std::vector<WindJob> WindJobs;
        std::vector<std::pair<GDX12UploadBuffer<XMFLOAT3>*, GDX12UploadBuffer<XMFLOAT3>*>> RootUploads;
        std::vector<std::unique_ptr<Output>> Outputs;
        std::map<std::tuple<uint64_t, GDX12GeometryStage, UINT>, Output*> OutputIndex;
        UINT DrawCursor = 0;
        UINT WindCursor = 0;
        std::unique_ptr<GDX12UploadBuffer<BistroInteractionSource>> Sources;
        std::unique_ptr<GDX12UploadBuffer<InteractionParameters>> InteractionConstants;
    };
    template<typename T> std::unique_ptr<GDX12UploadBuffer<T>> Buffer(UINT count, bool srv, bool uav);
    void Initialize();
    void UpdateWind(WindState& state, bool reset);
    void WindDispatch(UINT stage, const WindParameters& parameters,
        FieldBuffer* input, FieldBuffer* pressure, FieldBuffer* output);
    void RetireWind(WindState& state);
    Frame& CurrentFrame();
    Output* FindOutput(const GDX12GeometryContext&, UINT patch) const;
    static Stage RenderStage(GDX12GeometryStage);

    GDX12DeviceResources* _resources;
    GDX12GrassWindControls _controls;
    std::vector<Frame> _frames;
    std::unordered_map<uint64_t, GrassState> _grass;
    std::unordered_map<uint64_t, WindState> _wind;
    std::vector<uint64_t> _drawOrder;
    std::unique_ptr<GDX12RootSignature> _computeRoot, _graphicsRoot, _windRoot;
    ComPtr<ID3D12PipelineState> _expandPSO, _clearPSO;
    std::array<ComPtr<ID3D12PipelineState>, 6> _graphicsPSO;
    std::array<ComPtr<ID3D12PipelineState>, 7> _windPSO;
    std::unique_ptr<GDX12RootSignature> _interactionRoot;
    ComPtr<ID3D12PipelineState> _interactionPSO;
    std::unique_ptr<FieldBuffer> _presence, _trail, _previousTrail;
    static constexpr UINT InteractionResolution = 64;
    XMFLOAT4 _interactionArea = {};
    bool _resetInteraction = true;
    uint64_t _interactionRevision = 0;
    ComPtr<ID3D12CommandSignature> _signature;
    std::unique_ptr<GDX12UploadBuffer<Vertex>> _vertices;
    std::unique_ptr<GDX12UploadBuffer<UINT>> _indices, _arguments;
    std::unique_ptr<FieldBuffer> _neutralWind;
    Vector3 _previousEye = Vector3::Zero;
    Vector3 _eye = Vector3::Zero;
    bool _initialized = false;
    const Output* _arenaOutput = nullptr;
    static constexpr UINT ParametersPerBuffer = 64;
    GDX12RenderPrimitiveRegistry* _primitiveRegistry = nullptr;
    std::vector<GDX12RenderPrimitiveHandle> _removedPrimitives;
};

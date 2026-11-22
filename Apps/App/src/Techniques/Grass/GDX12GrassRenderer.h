#pragma once
#include <cstdint>
#include "GDX12GrassWind.h"
#include "Engine.RendererDX12/GDX12DeviceResources.h"
#include "Engine.RendererDX12/GDX12UploadBuffer.h"
#include "Engine.RendererDX12/GDX12RootSignature.h"
#include "Engine.RendererDX12/GDX12RenderGeometry.h"
#include <array>
#include "Components/BistroInteractionSource.h"
#include "Engine.RendererDX12/GDX12Particles.h"
#include <unordered_map>
#include <map>
#include <optional>

class GDX12CommandList;
class RenderPipelineCommonData;
class GDX12Texture;
namespace BistroRender { class SceneGeometry; class SceneGrass; }

// Owned by Bistro. Simulation may use COMPUTE; expansion and draw use DIRECT.
class GDX12GrassRenderer
{
public:
    enum class Stage : std::uint8_t { Full, GBuffer, PrepassNormals, Depth, Shadow, Capture };
    explicit GDX12GrassRenderer(GDX12DeviceResources* resources, UINT descriptorStart = ConstantsResources);
    ~GDX12GrassRenderer();
    // Wait the current frame-slot fence before reusing staging or replacing its roots.
    void BeginFrame(const RenderPipelineCommonData& common,
        std::vector<GDX12GrassInstance> grass, std::vector<GDX12WindInstance> wind, GDX12GrassWindControls controls,
        const std::vector<BistroInteractionSource>& sources = {}, float deltaSeconds = 0, uint64_t independentWind = 0,
        GDX12GrassRenderer* windDevice = nullptr, uint64_t responseWind = 0);
    GDX12ParticleInteractionField InteractionField() const;
    UINT PatchCount() const { return static_cast<UINT>(_drawOrder.size()); }
    void PublishPrimitives(GDX12RenderPrimitiveRegistry&, const IRenderGeometrySource*);
    void RemovePrimitives(GDX12RenderPrimitiveRegistry&);

private:
    friend class BistroRender::SceneGeometry;
    friend class BistroRender::SceneGrass;
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
        XMFLOAT3 PreviousEye;
        UINT ResetHistory;
        XMFLOAT4 Shape; // height, width, height variation, max bend
        XMFLOAT3 Lod; // near, far, bend seconds
        UINT InteractionSize;
        XMFLOAT2 PatchExtents;
        UINT HiZWidth, HiZHeight;
        XMFLOAT4 WindArea; // center xy, extents xy
        XMFLOAT4 InteractionArea;
        UINT Count, Segments, Implementation, Seed;
        UINT PointSource, Material, GridResolution, Shadow;
    };
    static_assert(sizeof(Parameters) == 336);
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
    using VelocityBuffers = std::array<std::unique_ptr<FieldBuffer>, 2>;
    struct WindSample
    {
        GDX12WindInstance Source;
        float Time = 0;
        float PreviousTime = 0;
        UINT Resolution = 0;
        bool ResetHistory = true;
        uint64_t SettingsHash = 0;
    };
    struct WindState : WindSample
    {
        VelocityBuffers Velocity, Pressure;
        std::unique_ptr<FieldBuffer> Advected, Divergence;
        bool Initialized = false;
        bool Remote = false;
    };
    struct WindJob
    {
        UINT Stage, Resolution;
        GDX12UploadBuffer<WindParameters>* Constants;
        FieldBuffer *Input, *Pressure, *Output;
        bool Async = false;
    };
    struct ViewInputs
    {
        const GrassState& State;
        GDX12UploadBuffer<Parameters>* Constants;
        UINT ConstantIndex;
        FieldBuffer *CurrentWind, *PreviousWind;
    };
    struct Frame
    {
        std::vector<std::unique_ptr<GDX12UploadBuffer<Parameters>>> DrawParameters;
        std::vector<std::unique_ptr<GDX12UploadBuffer<WindParameters>>> WindParametersBuffers;
        // Descriptors and upload mappings must survive every referencing command list.
        std::vector<std::unique_ptr<FieldBuffer>> RetiredFields;
        std::vector<std::unique_ptr<GDX12UploadBuffer<UINT>>> RetiredIndices;
        std::vector<GrassState> RetiredGrass;
        std::vector<WindJob> WindJobs;
        std::map<uint64_t, VelocityBuffers> ImportedWind;
        std::vector<std::pair<GDX12UploadBuffer<XMFLOAT3>*, GDX12UploadBuffer<XMFLOAT3>*>> RootUploads;
        UINT DrawCursor = 0;
        UINT WindCursor = 0;
        std::unique_ptr<GDX12UploadBuffer<BistroInteractionSource>> Sources;
        std::unique_ptr<GDX12UploadBuffer<InteractionParameters>> InteractionConstants;
    };
    template<typename T> std::unique_ptr<GDX12UploadBuffer<T>> Buffer(UINT count, bool srv, bool uav)
    {
        auto buffer = std::make_unique<GDX12UploadBuffer<T>>(_resources->Device, count, EBufferType::Default, false);
        auto* heap = _resources->SRV_UAV_Heap.get();
        if (srv) buffer->CreateSRV(heap, heap->GetAvailableIndex(_descriptorStart));
        if (uav) buffer->CreateUAV(heap, heap->GetAvailableIndex(_descriptorStart));
        return buffer;
    }
    void Initialize();
    void InitializeGraphics(bool captureOnly = false);
    void InitializeWind();
    void EnsureNeutralWind();
    void PlanLocalWind(WindState&, uint64_t independentWind);
    void FallBackToLocalWind(uint64_t independentWind);
    void ApplyWindHistory();
    const VelocityBuffers& Velocity(const WindState&);
    void UpdateWind(WindState& state, bool reset);
    void WindDispatch(UINT stage, const WindParameters& parameters,
        FieldBuffer* input, FieldBuffer* pressure, FieldBuffer* output);
    void RetireWind(WindState& state);
    Frame& CurrentFrame();
    std::optional<ViewInputs> PrepareViewInputs(const GDX12GeometryContext&, UINT patch);
    GDX12GraphicsBindings DrawBindings(const GDX12GeometryContext&, D3D12_GPU_VIRTUAL_ADDRESS parameters,
        D3D12_GPU_DESCRIPTOR_HANDLE vertices, D3D12_GPU_DESCRIPTOR_HANDLE materials,
        D3D12_GPU_VIRTUAL_ADDRESS main, D3D12_GPU_DESCRIPTOR_HANDLE localLights) const;
    static Stage RenderStage(GDX12GeometryStage);

    GDX12DeviceResources* _resources;
    UINT _descriptorStart;
    GDX12GrassWindControls _controls;
    std::vector<Frame> _frames;
    std::unordered_map<uint64_t, GrassState> _grass;
    std::unordered_map<uint64_t, WindState> _wind;
    std::vector<uint64_t> _drawOrder;
    std::unique_ptr<GDX12RootSignature> _graphicsRoot, _windRoot;
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
    std::unique_ptr<GDX12UploadBuffer<UINT>> _indices, _arguments;
    std::unique_ptr<FieldBuffer> _neutralWind;
    Vector3 _previousEye = Vector3::Zero;
    Vector3 _eye = Vector3::Zero;
    bool _initialized = false;
    static constexpr UINT ParametersPerBuffer = 64;
    GDX12RenderPrimitiveRegistry* _primitiveRegistry = nullptr;
    std::vector<GDX12RenderPrimitiveHandle> _removedPrimitives;
};

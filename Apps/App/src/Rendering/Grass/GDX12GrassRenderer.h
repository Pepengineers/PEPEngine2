#pragma once
#include <cstdint>
#include "Rendering/Grass/GDX12GrassFieldProducer.h"
#include "Engine.RendererDX12/GDX12UploadBuffer.h"
#include "Engine.RendererDX12/GDX12RootSignature.h"
#include "Engine.RendererDX12/GDX12RenderGeometry.h"
#include <array>
#include "Components/BistroInteractionSource.h"
#include "Engine.RendererDX12/GDX12Particles.h"
#include <unordered_map>
#include <unordered_set>
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
    struct DisplayedFields
    {
        std::shared_ptr<const GDX12GrassFieldSnapshot> Current, Previous;
        bool ResetHistory = false;
    };

    struct LocalCheckpoint
    {
        GDX12GrassFieldProducer::SnapshotLayout Layout;
        std::shared_ptr<GDX12Resource> Packed;
    };

    struct ShadowPatch
    {
        GDX12RenderPrimitiveHandle Handle;
        GDX12GrassInstance Instance;
        BoundingBox Bounds;
    };
    struct ShadowMaterials
    {
        GDX12DescriptorHeap* Heap = nullptr;
        GDX12Resource* Buffer = nullptr;
        D3D12_GPU_DESCRIPTOR_HANDLE Table = {};
        D3D12_GPU_DESCRIPTOR_HANDLE AlphaTable = {};
        UINT Count = 0;
    };
    std::optional<std::vector<ShadowPatch>> FreezeShadowPatches(
        const GDX12RenderPrimitiveRegistry& registry, const IRenderGeometrySource& source) const;
    GDX12GrassRenderer(GDX12DeviceResources* resources, ShadowMaterials materials);
    void SetShadowMaterials(ShadowMaterials materials);

    explicit GDX12GrassRenderer(GDX12DeviceResources* resources);
    ~GDX12GrassRenderer();
    // Wait the current frame-slot fence before reusing staging or replacing its roots.
    void BeginFrame(const RenderPipelineCommonData& common,
        std::vector<GDX12GrassInstance> grass, std::vector<GDX12WindInstance> wind, GDX12GrassWindControls controls,
        const std::vector<BistroInteractionSource>& sources = {}, float deltaSeconds = 0,
        const DisplayedFields* fields = nullptr);
    static XMFLOAT4 BistroInteractionArea();
    static uint64_t FieldInputSignature(const std::vector<GDX12GrassInstance>& grass, const GDX12GrassWindControls& controls);
    GDX12ParticleInteractionField InteractionField() const;
    // Request before recording this frame's local preparation pass.
    std::shared_ptr<const LocalCheckpoint> PrepareLocalCheckpoint();
    void RestartLocalSolver();
    // Caller has observed the seed receipt; BeginFrame may now advance this solver.
    void ResumeLocalSolver(std::shared_ptr<GDX12GrassFieldProducer> solver, uint64_t interactionResetRevision,
        GDX12GrassFieldInputQueue pending = {});
    void PrepareGPU(GDX12CommandList* commands);
    void PrepareView(const GDX12GeometryContext& view, UINT patch);
    // A placed caller can provide the packed views' NON_PIXEL_SHADER_RESOURCE state in its graph.
    bool Expand(GDX12CommandList* commands, const GDX12GeometryContext& view, UINT patch,
        bool packedFieldStateDeclared = false);
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
    using FieldView = GDX12GrassFieldSnapshot::View;
    struct Output
    {
        GDX12GeometryContext View;
        GDX12CaptureLighting Lighting;
        UINT Patch = 0;
        uint64_t Owner = 0;
        GDX12UploadBuffer<Parameters>* Constants = nullptr;
        UINT ConstantIndex = 0;
        std::array<FieldView, 4> Fields;
        GDX12GraphicsBindings Binding;
        GDX12RenderBatch Batch;
    };
    struct Frame
    {
        std::vector<std::unique_ptr<GDX12UploadBuffer<Parameters>>> DrawParameters;
        // Descriptors and upload mappings must survive every referencing command list.
        std::vector<std::unique_ptr<GDX12UploadBuffer<Vertex>>> RetiredVertices;
        std::vector<std::unique_ptr<GDX12UploadBuffer<UINT>>> RetiredIndices;
        std::vector<GrassState> RetiredGrass;
        std::vector<std::pair<GDX12UploadBuffer<XMFLOAT3>*, GDX12UploadBuffer<XMFLOAT3>*>> RootUploads;
        std::vector<std::unique_ptr<Output>> Outputs;
        std::map<std::tuple<uint64_t, GDX12GeometryStage, UINT>, Output*> OutputIndex;
        UINT DrawCursor = 0;
        DisplayedFields Snapshots;
        std::shared_ptr<GDX12GrassFieldProducer> LocalSolver;
        std::shared_ptr<const LocalCheckpoint> Checkpoint;
        std::unordered_set<uint64_t> SnapshotWindOwners;
    };
    template<typename T> std::unique_ptr<GDX12UploadBuffer<T>> Buffer(UINT count, bool srv, bool uav);
    void Initialize();
    Frame& CurrentFrame();
    GDX12DescriptorHeap* BindingHeap() const;
    Output* FindOutput(const GDX12GeometryContext&, UINT patch) const;
    static Stage RenderStage(GDX12GeometryStage);

    GDX12DeviceResources* _resources;
    std::optional<ShadowMaterials> _shadowMaterials;
    GDX12GrassWindControls _controls;
    std::vector<Frame> _frames;
    std::unordered_map<uint64_t, GrassState> _grass;
    std::shared_ptr<GDX12GrassFieldProducer> _fieldProducer;
    bool _usingSnapshot = false;
    bool _resumeSeed = false;
    bool _restartLocal = false;
    bool _carryWindDebt = false;
    std::optional<GDX12GrassFieldInputQueue> _localContinuation;
    uint64_t _interactionResetRevision = 0;
    std::vector<uint64_t> _drawOrder;
    std::unique_ptr<GDX12RootSignature> _computeRoot, _graphicsRoot;
    ComPtr<ID3D12PipelineState> _expandPSO, _clearPSO;
    std::array<ComPtr<ID3D12PipelineState>, 6> _graphicsPSO;
    ComPtr<ID3D12CommandSignature> _signature;
    std::unique_ptr<GDX12UploadBuffer<Vertex>> _vertices;
    std::unique_ptr<GDX12UploadBuffer<UINT>> _indices, _arguments;
    Vector3 _previousEye = Vector3::Zero;
    Vector3 _eye = Vector3::Zero;
    bool _initialized = false;
    const Output* _arenaOutput = nullptr;
    static constexpr UINT ParametersPerBuffer = 64;
    GDX12RenderPrimitiveRegistry* _primitiveRegistry = nullptr;
    std::vector<GDX12RenderPrimitiveHandle> _removedPrimitives;
};

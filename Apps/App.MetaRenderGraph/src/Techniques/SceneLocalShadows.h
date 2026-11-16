#pragma once
#include "NativeWork.h"
#include "GeometryTarget.h"
#include "Engine.Core/Jobs/JobSystem.h"
#include "Engine.RendererDX12/GDX12DeviceResources.h"
#include <optional>
#include <functional>
#include <unordered_map>

class RenderPipelineCommonData;

namespace Meta
{
class SceneLocalShadows
{
public:
    SceneLocalShadows(GDX12DeviceResources&, RenderPipelineCommonData&, bool staticOnly = false);
    ~SceneLocalShadows();
    void Freeze(UINT slot, const std::vector<GDX12RenderPrimitive>& grass = {}, bool allReceivers = false);
    void Complete(UINT slot);
    void Submitted(UINT slot);
    uint64_t SceneRevision() const { return _sceneRevision; }
    const std::vector<BoundingBox>& ChangedCasterBounds() const { return _changedCasterBounds; }
    bool CastersRebuilt() const { return _castersRebuilt; }
    GDX12UploadBuffer<GDX12LocalLightConstants>& LightSource(UINT slot) { return *_slots.at(slot).LightUpload; }
    void Append(Graph&, std::vector<ID3D12Resource*>&, UINT slot,
        const std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 4>& scene,
        GDX12GeometryBuffer::RecordingBinding geometry, GDX12UploadBuffer<GDX12LocalLightConstants>& lights,
        const std::function<void(const GeometryTarget&, Graph&, std::vector<ID3D12Resource*>&)>& appendGrass = {});
    bool Record(UINT slot, Id id, ID3D12GraphicsCommandList* cmd) const { return _slots.at(slot).Work.Record(id, cmd); }
    GDX12Texture* Output(UINT slot) { return &_slots.at(slot).Atlas; }
private:
    struct Input
    {
        GDX12IndirectDrawArgs Command{};
        GDX12InstanceData Instance{};
        GDX12MaterialConstants Material;
        XMFLOAT4X4 World{};
        uint64_t DeformationRevision = 0;
        bool Dynamic = false;
    };
    struct Caster { BoundingBox Bounds; GDX12IndirectDrawArgs Command; bool CastsShadow, Masked; UINT InputIndex; bool Dynamic; };
    struct Node { BoundingBox Bounds; UINT First = 0, Count = 0, Left = 0, Right = 0, Parent = UINT_MAX; };
    struct FaceCommands { std::vector<GDX12IndirectDrawArgs> Opaque, Masked; };
    struct LightCache
    {
        GDX12LocalLightConstants Light;
        std::array<FaceCommands, 6> StaticFaces, DynamicFaces;
        UINT FirstSlice = UINT_MAX, Needed = 0, StaticListsValid = 0, DynamicListsValid = 0, MapsValid = 0;
        bool HasLight = false;
        bool SelectionDirty = true;
    };
    struct View { UINT Light, Face, Slice, First, OpaqueCount, MaskedCount; };
    struct Slot;
    struct CullJob final : ParallelJob
    {
        SceneLocalShadows* Owner = nullptr;
        Slot* Frame = nullptr;
        void Execute(size_t index) override;
    };
    struct PackJob final : ParallelJob
    {
        Slot* Frame = nullptr;
        void Execute(size_t) override;
    };
    struct Slot
    {
        std::vector<GDX12LocalLightConstants> Lights;
        std::vector<LightCache> Cache;
        std::vector<View> Views;
        std::vector<GDX12IndirectDrawArgs> Packed;
        std::optional<GDX12LocalShadowFrustum> Camera;
        XMFLOAT4X4 CameraMatrix{};
        bool SelectionChanged = true, AllReceivers = false;
        std::array<XMFLOAT4, 6> CameraPlanes;
        std::array<XMFLOAT3, 8> CameraCorners;
        CullJob Cull;
        PackJob Pack;
        JobHandle Ready;
        std::unique_ptr<GDX12UploadBuffer<GDX12LocalLightConstants>> LightUpload;
        std::unique_ptr<GDX12UploadBuffer<GDX12IndirectDrawArgs>> CommandUpload, Commands;
        GDX12Texture Atlas;
        std::vector<std::unique_ptr<GDX12Descriptor>> Dsvs;
        UINT Resolution = 0, Slices = 0, NextSlice = 0;
        uint64_t SceneRevision = 0, GrassReceiverRevision = 0;
        NativeWork Work;
    };
    UINT BuildNode(UINT first, UINT count, UINT parent = UINT_MAX);
    void Refit(UINT caster);
    void Invalidate(const Caster&, bool listsChanged);
    void InvalidateGrass(const GDX12RenderPrimitive&);
    void UpdateGrass(const std::vector<GDX12RenderPrimitive>&);
    void UpdateCasters(UINT slot);
    void Gather(UINT node, const GDX12LocalLightConstants&, const std::array<GDX12LocalShadowFrustum, 6>&,
        UINT faceMask, std::array<FaceCommands, 6>&) const;
    UINT NeededFaces(UINT node, const GDX12LocalLightConstants&, const std::array<GDX12LocalShadowFrustum, 6>&,
        UINT faceMask, const Slot&) const;
    void RebuildCasters(UINT slot);
    Input ReadInput(UINT slot, UINT command) const;
    void PrepareAtlas(Slot&, UINT slices);
    GDX12DeviceResources& _resources;
    RenderPipelineCommonData& _common;
    std::unique_ptr<GDX12DescriptorHeap> _dsvHeap;
    std::vector<Slot> _slots;
    std::vector<Caster> _casters;
    std::vector<BoundingBox> _changedCasterBounds;
    std::vector<Node> _nodes;
    UINT _staticRoot = UINT_MAX, _dynamicRoot = UINT_MAX;
    std::vector<Input> _inputs;
    std::vector<GDX12RenderPrimitive> _grass;
    std::unordered_map<UINT, std::vector<UINT>> _instances, _transforms, _materials;
    std::vector<UINT> _positions, _leaves;
    bool _treeReady = false, _castersRebuilt = false;
    bool _staticOnly = false;
    uint64_t _sceneRevision = 0, _geometryRevision = 0, _meshRevision = 0, _textureRevision = 0;
    uint64_t _grassReceiverRevision = 0;
    std::unique_ptr<GDX12RootSignature> _root;
    ComPtr<ID3D12PipelineState> _opaqueDraw, _maskedDraw;
    ComPtr<ID3D12CommandSignature> _signature;
};
}

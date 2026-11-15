#pragma once
#include "NativeWork.h"
#include "Engine.Core/Jobs/JobSystem.h"
#include "Engine.RendererDX12/GDX12DeviceResources.h"
#include <optional>
#include <unordered_map>

class RenderPipelineCommonData;

namespace Meta
{
class SceneLocalShadows
{
public:
    SceneLocalShadows(GDX12DeviceResources&, RenderPipelineCommonData&);
    ~SceneLocalShadows();
    void Freeze(UINT slot, bool sceneChanged);
    void Complete(UINT slot);
    void Submitted(UINT slot);
    GDX12UploadBuffer<GDX12LocalLightConstants>& LightSource(UINT slot) { return *_slots.at(slot).LightUpload; }
    void Append(Graph&, std::vector<ID3D12Resource*>&, UINT slot,
        const std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 4>& scene,
        GDX12GeometryBuffer::RecordingBinding geometry, GDX12UploadBuffer<GDX12LocalLightConstants>& lights);
    bool Record(UINT slot, Id id, ID3D12GraphicsCommandList* cmd) const { return _slots.at(slot).Work.Record(id, cmd); }
    GDX12Texture* Output(UINT slot) { return &_slots.at(slot).Atlas; }
private:
    struct Input
    {
        GDX12IndirectDrawArgs Command{};
        GDX12InstanceData Instance{};
        GDX12MaterialConstants Material;
        XMFLOAT4X4 World{};
    };
    struct Caster { BoundingBox Bounds; GDX12IndirectDrawArgs Command; bool CastsShadow, Masked; };
    struct Node { BoundingBox Bounds; UINT First = 0, Count = 0, Left = 0, Right = 0; };
    struct FaceCommands { std::vector<GDX12IndirectDrawArgs> Opaque, Masked; };
    struct LightCache
    {
        GDX12LocalLightConstants Light;
        std::array<FaceCommands, 6> Faces;
        UINT FirstSlice = UINT_MAX, Needed = 0, ListsValid = 0, MapsValid = 0;
        bool HasLight = false;
    };
    struct View { UINT Light, Face, Slice, First, OpaqueCount, MaskedCount; };
    struct Slot;
    struct ReceiverJob final : ParallelJob
    {
        SceneLocalShadows* Owner = nullptr;
        Slot* Frame = nullptr;
        void Execute(size_t index) override;
    };
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
        std::vector<std::optional<BoundingBox>> Receivers;
        std::optional<GDX12LocalShadowFrustum> Camera;
        std::array<XMFLOAT4, 6> CameraPlanes;
        std::array<XMFLOAT3, 8> CameraCorners;
        ReceiverJob ReceiversJob;
        CullJob Cull;
        PackJob Pack;
        JobHandle Ready;
        std::unique_ptr<GDX12UploadBuffer<GDX12LocalLightConstants>> LightUpload;
        std::unique_ptr<GDX12UploadBuffer<GDX12IndirectDrawArgs>> CommandUpload, Commands;
        GDX12Texture Atlas;
        std::vector<std::unique_ptr<GDX12Descriptor>> Dsvs;
        UINT Resolution = 0, Slices = 0, NextSlice = 0;
        uint64_t SceneRevision = 0;
        NativeWork Work;
    };
    UINT BuildNode(UINT first, UINT count);
    void Gather(UINT node, const GDX12LocalLightConstants&, const std::array<GDX12LocalShadowFrustum, 6>&,
        UINT faceMask, LightCache&) const;
    UINT NeededFaces(UINT node, const GDX12LocalLightConstants&, const std::array<GDX12LocalShadowFrustum, 6>&,
        UINT faceMask, const Slot&) const;
    void RebuildCasters(UINT slot);
    Input ReadInput(UINT slot, UINT command) const;
    bool SceneInputsChanged(UINT slot) const;
    bool TextureBindingsChanged();
    void PrepareAtlas(Slot&, UINT slices);
    GDX12DeviceResources& _resources;
    RenderPipelineCommonData& _common;
    std::vector<Slot> _slots;
    std::vector<Caster> _casters;
    std::vector<Node> _nodes;
    std::vector<Input> _inputs;
    std::unordered_map<std::string, ComPtr<ID3D12Resource>> _textureResources;
    bool _treeReady = false;
    uint64_t _sceneRevision = 0, _geometryRevision = 0, _meshRevision = 0, _materialRevision = 0;
    std::unique_ptr<GDX12RootSignature> _root;
    ComPtr<ID3D12PipelineState> _opaqueDraw, _maskedDraw;
    ComPtr<ID3D12CommandSignature> _signature;
};
}

#pragma once
#include "NativeWork.h"
#include "Engine.Core/Jobs/JobSystem.h"
#include "Engine.RendererDX12/GDX12DeviceResources.h"
#include <optional>

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
    GDX12UploadBuffer<GDX12LocalLightConstants>& LightSource(UINT slot) { return *_slots.at(slot).LightUpload; }
    void Append(Graph&, std::vector<ID3D12Resource*>&, UINT slot,
        const std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 4>& scene,
        GDX12GeometryBuffer::RecordingBinding geometry, GDX12UploadBuffer<GDX12LocalLightConstants>& lights);
    bool Record(UINT slot, Id id, ID3D12GraphicsCommandList* cmd) const { return _slots.at(slot).Work.Record(id, cmd); }
    GDX12Texture* Output(UINT slot) { return &_slots.at(slot).Atlas; }
private:
    struct Caster { BoundingBox Bounds; GDX12IndirectDrawArgs Command; };
    struct Node { BoundingBox Bounds; UINT First = 0, Count = 0, Left = 0, Right = 0; };
    struct LightWork { std::array<std::vector<GDX12IndirectDrawArgs>, 6> Faces; };
    struct View { UINT Light, Face, Slice, First, Count; };
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
        std::vector<LightWork> Lists;
        std::vector<View> Views;
        std::vector<GDX12IndirectDrawArgs> Packed;
        std::optional<GDX12LocalShadowFrustum> Camera;
        CullJob Cull;
        PackJob Pack;
        JobHandle Ready;
        std::unique_ptr<GDX12UploadBuffer<GDX12LocalLightConstants>> LightUpload;
        std::unique_ptr<GDX12UploadBuffer<GDX12IndirectDrawArgs>> CommandUpload, Commands;
        GDX12Texture Atlas;
        std::vector<std::unique_ptr<GDX12Descriptor>> Dsvs;
        UINT Resolution = 0, Slices = 0;
        NativeWork Work;
    };
    UINT BuildNode(UINT first, UINT count);
    void Gather(UINT node, const GDX12LocalLightConstants&, const std::array<GDX12LocalShadowFrustum, 6>&,
        UINT faceMask, LightWork&) const;
    void RebuildCasters(UINT slot);
    void PrepareAtlas(Slot&);
    GDX12DeviceResources& _resources;
    RenderPipelineCommonData& _common;
    std::vector<Slot> _slots;
    std::vector<Caster> _casters;
    std::vector<Node> _nodes;
    bool _treeReady = false;
    std::unique_ptr<GDX12RootSignature> _root;
    ComPtr<ID3D12PipelineState> _draw;
    ComPtr<ID3D12CommandSignature> _signature;
};
}

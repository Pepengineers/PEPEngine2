#pragma once
#include "NativeWork.h"
#include "GeometryTarget.h"
#include "Engine.RendererDX12/GDX12DeviceResources.h"
#include <functional>

class RenderPipelineCommonData;

namespace Meta
{
class SceneSunShadows
{
public:
    SceneSunShadows(GDX12DeviceResources&, RenderPipelineCommonData&);
    void Freeze(UINT slot, bool sceneChanged, const std::vector<GDX12RenderPrimitive>& grass = {});
    void Append(Graph&, std::vector<ID3D12Resource*>&, UINT slot,
        const std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 4>& scene, UINT draws,
        GDX12GeometryBuffer::RecordingBinding geometry,
        const std::function<void(const GeometryTarget&, Graph&, std::vector<ID3D12Resource*>&)>& appendGrass = {});
    bool Record(UINT slot, Id id, ID3D12GraphicsCommandList* cmd) const { return _slots.at(slot).Work.Record(id, cmd); }
    GDX12Texture* Output(UINT slot) { return &_slots.at(slot).Atlas; }
private:
    struct Slot
    {
        GDX12Texture Atlas;
        std::array<std::unique_ptr<GDX12UploadBuffer<GDX12IndirectDrawArgs>>, 4> Commands;
        std::unique_ptr<GDX12UploadBuffer<UINT>> Counters;
        NativeWork Work;
        BoundingBox Bounds{};
        bool HasBounds = false, BoundsReady = false, Rendered = false, Update = false;
        UINT Resolution = UINT_MAX, Revision = 0;
        uint64_t GrassRevision = 0;
        GDX12ShadowConstants Settings{};
    };
    GDX12DeviceResources& _resources;
    RenderPipelineCommonData& _common;
    std::vector<Slot> _slots;
    std::unique_ptr<GDX12RootSignature> _graphicsRoot, _computeRoot;
    ComPtr<ID3D12PipelineState> _draw, _cull, _clear;
    ComPtr<ID3D12CommandSignature> _signature;
};
}

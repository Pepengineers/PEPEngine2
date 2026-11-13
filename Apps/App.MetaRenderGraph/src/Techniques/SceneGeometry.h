#pragma once
#include "Graph/MetaGraphExecutor.h"
#include "Engine.RendererDX12/GDX12DeviceResources.h"
#include "Engine.RendererDX12/GDX12Skinning.h"
#include <array>

namespace Meta {
// Owner-only Freeze/Resize. Record uses immutable slot data and native commands;
// no shared resource tracker, heap allocator, ECS or queue access on workers.
class SceneGeometry {
public:
    explicit SceneGeometry(GDX12DeviceResources&);
    void Resize(UINT width, UINT height); // caller has drained all slots
    void Freeze(UINT slot, UINT camera, std::vector<GDX12SkinningWork> skin);
    Schedule Plan(UINT slot) const;
    std::vector<ID3D12Resource*> Resources(UINT slot, ID3D12Resource* target) const;
    void Record(UINT slot, Id operation, UINT bucket, ID3D12GraphicsCommandList*, ID3D12Resource* target) const;
    std::vector<uint8_t> Readback(UINT slot, const std::string& stem) const; // after GPU completion
private:
    struct Buffer {
        GDX12ResourceHandle Source, Data;
        UINT64 Bytes=0;
        GDX12Descriptor Srv, Uav;
    };
    struct Slot {
        std::array<Buffer,8> Buffers; // instances/templates/materials/transforms/opaque/count/transparent/count
        std::array<std::unique_ptr<GDX12Texture>,5> Color;
        GDX12ResourceHandle Depth;
        GDX12Descriptor Dsv, ReadDepth;
        GDX12GeometryBuffer::RecordingBinding Geometry{};
        D3D12_GPU_VIRTUAL_ADDRESS Camera=0, Main=0;
        UINT Draws=0;
        GDX12ResourceHandle ImageReadback, SceneReadback;
        std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT,6> Footprints{};
        UINT64 ImageBytes=0;
        std::array<UINT64,8> Offsets{};
        UINT64 SceneBytes=0;
        UINT64 SkinOffset=0;
        std::vector<GDX12SkinningWork> Skin;
        std::vector<ID3D12Resource*> SkinBindings;
        std::vector<Resource> SkinDeclarations;
        std::vector<std::array<Id,5>> SkinIds;
    };
    void BufferFor(Buffer&, UINT64 bytes, UINT stride, bool upload=false);
    GDX12DeviceResources& _resources;
    std::vector<Slot> _slots;
    std::unique_ptr<GDX12RootSignature> _cullRoot, _clearRoot, _drawRoot;
    ComPtr<ID3D12PipelineState> _cull, _clear, _depth, _gbuffer;
    ComPtr<ID3D12CommandSignature> _signature;
    UINT _width=0,_height=0;
};
}

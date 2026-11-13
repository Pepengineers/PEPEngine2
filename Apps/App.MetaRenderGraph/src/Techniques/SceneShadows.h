#pragma once
#include "Graph/MetaGraph.h"
#include "Engine.RendererDX12/GDX12DeviceResources.h"
#include "GeometryTarget.h"

namespace Meta {
class SceneShadows {
public:
    explicit SceneShadows(GDX12DeviceResources&);
    void Freeze(UINT slot, const std::array<D3D12_GPU_DESCRIPTOR_HANDLE,4>& scene, UINT draws,
        GDX12GeometryBuffer::RecordingBinding geometry);
    void Append(Graph&, UINT slot);
    std::vector<ID3D12Resource*> Resources(UINT slot) const;
    bool Record(UINT slot, Id, UINT bucket, ID3D12GraphicsCommandList*) const;
    std::vector<uint8_t> Readback(UINT slot,const std::string& stem) const;
    std::vector<GeometryTarget> Targets(UINT slot) const;
    void RepeatReadbacks(Graph&,UINT slot);
private:
    struct View {
        UINT Resolution=0; bool Sun=false;
        std::unique_ptr<GDX12UploadBuffer<GDX12LocalLightConstants>> Constants;
        std::array<std::unique_ptr<GDX12UploadBuffer<GDX12IndirectDrawArgs>>,2> Commands;
        std::array<std::unique_ptr<GDX12UploadBuffer<UINT>>,2> Counts;
        GDX12ResourceHandle Depth, Readback;
        GDX12Descriptor Dsv;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT Footprint{};
        UINT64 DepthBytes=0;
        Matrix ViewProjection;
        Id DepthId=Invalid;
    };
    struct Slot {
        std::vector<std::unique_ptr<View>> Views;
        std::array<D3D12_GPU_DESCRIPTOR_HANDLE,4> Scene;
        GDX12GeometryBuffer::RecordingBinding Geometry;
        UINT Draws=0; Id FirstOperation=Invalid;
        std::vector<std::pair<Id,Id>> Repeated;
    };
    GDX12DeviceResources& _resources;
    std::vector<Slot> _slots;
    std::unique_ptr<GDX12RootSignature> _computeRoot,_graphicsRoot;
    ComPtr<ID3D12PipelineState> _clear;
    std::array<ComPtr<ID3D12PipelineState>,2> _cull,_draw;
    ComPtr<ID3D12CommandSignature> _signature;
};
}

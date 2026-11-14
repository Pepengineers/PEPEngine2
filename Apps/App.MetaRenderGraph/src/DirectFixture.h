#pragma once
#include "Graph/MetaGraphExecutor.h"
#include "Engine.RendererDX12/GDX12ResourceHandle.h"
namespace Meta {
// Real upload/three dependent dispatches/diamond reads/draw/readback fixture.
// Fixed buffers, root descriptors and lists; no transient allocation during recording.
class DirectFixture {
public:
    DirectFixture(GDX12Device&, uint32_t slots);
    void Prepare(uint32_t slot, uint32_t seed);
    Schedule Plan(State targetInitial, State targetFinal, bool asyncQueues = false) const;
    std::vector<ID3D12Resource*> Resources(uint32_t slot, ID3D12Resource* target) const;
    void Record(uint32_t slot, Id operation, uint32_t list, ID3D12GraphicsCommandList*,
        ID3D12Resource* target, D3D12_CPU_DESCRIPTOR_HANDLE rtv, uint32_t width, uint32_t height);
    void Validate(uint32_t slot) const; // caller must wait for executor slot
private:
    struct Slot {
        GDX12ResourceHandle Upload, Data, Readback, A, B, Pixel, Independent, IndependentReadback;
        uint32_t Seed=0; bool Used=false;
    };
    GDX12Device& _device;
    std::vector<Slot> _slots;
    ComPtr<ID3D12RootSignature> _root;
    ComPtr<ID3D12PipelineState> _compute, _graphics;
};
}

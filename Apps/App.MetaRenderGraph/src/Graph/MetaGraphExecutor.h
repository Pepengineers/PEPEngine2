#pragma once
#include "MetaGraph.h"
#include "Engine.RendererDX12/GDX12Device.h"
#include <functional>
#include <thread>
class GDX12CommandList;
namespace Meta {
// Single owner thread. Native command recording cannot call tracker-mutating
// GDX12Resource helpers. All resources/descriptors outlive the slot fence.
class Executor {
public:
    using Record = std::function<void(Id, uint32_t, ID3D12GraphicsCommandList*)>;
    Executor(GDX12Device&, uint32_t slots, uint32_t listCapacity);
    ~Executor();
    void Begin(uint32_t slot);
    uint64_t Execute(const Schedule&, const std::vector<ID3D12Resource*>&, const Record&,
        GDX12CommandList* prefix = nullptr, GDX12CommandList* suffix = nullptr);
    void Drain();
    const std::vector<uint64_t>& Tokens() const { return _tokens; }
    uint64_t Retirement(uint32_t slot) const { return _slots.at(slot).Fence; }
private:
    struct List { ComPtr<ID3D12CommandAllocator> Allocator; ComPtr<ID3D12GraphicsCommandList> Commands; };
    struct Slot { std::vector<List> Lists; uint64_t Fence=0; bool Poisoned=false; };
    void Owner() const;
    GDX12Device& _device;
    std::thread::id _owner=std::this_thread::get_id();
    std::vector<Slot> _slots;
    std::vector<uint64_t> _tokens;
    uint32_t _slot=0;
    bool _begun=false;
};
}

#pragma once
#include "MetaGraph.h"
#include "MetaGraphRecorder.h"
#include "Engine.RendererDX12/GDX12Device.h"
#include <functional>
#include <thread>
class GDX12CommandList;
namespace Meta {
// Begin/submission/drain have one owner thread; Record callbacks run on workers.
// Callbacks cannot submit, wait for GPU, or call tracker-mutating helpers.
// Caller freezes resources/descriptors/upload ranges through the slot fence;
// Begin is the only point permitting their rewrite. After a partial error drain
// and recreate the executor and resources (texture final states may not hold).
class Executor {
public:
    using Record = std::function<void(Id, uint32_t, ID3D12GraphicsCommandList*)>;
    Executor(GDX12Device&, uint32_t slots, uint32_t listCapacity, unsigned threads = 4);
    ~Executor();
    void Begin(uint32_t slot);
    // Explicit owner-only capacity growth after Begin retired the selected slot.
    // Execute still rejects an unreserved plan; workers never allocate lists.
    void Reserve(uint32_t lists);
    uint64_t Execute(const Schedule&, const std::vector<ID3D12Resource*>&, const Record&,
        GDX12CommandList* prefix = nullptr, GDX12CommandList* suffix = nullptr,
        bool serial = false, const std::atomic<bool>* cancellation = nullptr,
        std::ostream* trace = nullptr);
    void Drain();
    const std::vector<uint64_t>& Tokens() const { return _tokens; }
    uint64_t Retirement(uint32_t slot) const { return _slots.at(slot).Fence; }
private:
    struct List { ComPtr<ID3D12CommandAllocator> Allocator; ComPtr<ID3D12GraphicsCommandList> Commands; };
    struct Slot { std::vector<List> Lists; uint64_t Fence=0; bool Poisoned=false; };
    void Owner() const;
    GDX12Device& _device;
    JobSystem _jobs;
    std::thread::id _owner=std::this_thread::get_id();
    std::vector<Slot> _slots;
    std::vector<uint64_t> _tokens;
    uint32_t _slot=0;
    bool _begun=false;
    bool _aborted=false; // partial frame requires resource-state rebuild
};
}

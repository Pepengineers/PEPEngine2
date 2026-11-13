#include "MetaGraphExecutor.h"
#include "Engine.RendererDX12/GDX12CommandQueue.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include <stdexcept>
namespace Meta {
Executor::Executor(GDX12Device& d,uint32_t slots,uint32_t capacity):_device(d),_slots(slots) {
    if(!slots || !capacity) throw std::invalid_argument("empty executor capacity");
    for(auto& slot:_slots) for(uint32_t i=0;i<capacity;++i) {
        List list;
        ThrowIfFailed(d.GetDevice()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&list.Allocator)));
        ThrowIfFailed(d.GetDevice()->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,list.Allocator.Get(),nullptr,IID_PPV_ARGS(&list.Commands)));
        ThrowIfFailed(list.Commands->Close());
        slot.Lists.push_back(std::move(list));
    }
}
Executor::~Executor() {
    try { Drain(); } catch(...) {
        // No known successful completion: retain allocator/list references.
        for(auto& s:_slots) for(auto& l:s.Lists) { l.Commands.Detach(); l.Allocator.Detach(); }
    }
}
void Executor::Owner() const { if(std::this_thread::get_id()!=_owner) throw std::logic_error("executor requires owner thread"); }
void Executor::Begin(uint32_t index) {
    Owner();
    auto& slot=_slots.at(index);
    if(slot.Poisoned) throw std::logic_error("executor slot quarantined after submission failure");
    _device.GetCommandQueue()->CPUWaitForFenceValue(slot.Fence);
    _slot=index; _begun=true; _tokens.clear();
}
void Executor::Drain() {
    Owner();
    _device.GetCommandQueue()->Flush();
    for(auto& s:_slots) if(s.Poisoned) throw std::logic_error("submission failed; executor quarantined");
}
uint64_t Executor::Execute(const Schedule& schedule,const std::vector<ID3D12Resource*>& resources,
    const Record& record,GDX12CommandList* prefix,GDX12CommandList* suffix) {
    Owner();
    if(!_begun) throw std::logic_error("Begin required");
    _begun=false;
    auto& slot=_slots[_slot];
    _tokens.assign(schedule.Packets.size(),0);
    size_t total=0; for(auto& p:schedule.Packets) total+=p.Lists;
    if(total>slot.Lists.size()) throw std::length_error("fixed command list capacity exceeded");
    std::vector<std::vector<ID3D12CommandList*>> packets;
    size_t cursor=0;
    // Record and close everything before any submission. A recording/Close
    // failure cannot execute a partial packet or a dependent draw.
    for(Id i=0;i<schedule.Packets.size();++i) {
        const auto& p=schedule.Packets[i];
        for(auto dep:p.Dependencies) if(dep>=i) throw std::invalid_argument("non-topological packet dependency");
        packets.emplace_back();
        for(uint32_t j=0;j<p.Lists;++j) {
            auto& l=slot.Lists[cursor++];
            ThrowIfFailed(l.Allocator->Reset());
            ThrowIfFailed(l.Commands->Reset(l.Allocator.Get(),nullptr));
            try {
                auto barriers=[&](const std::vector<Barrier>& bs) {
                    for(auto b:bs) {
                        auto* r=resources.at(b.Resource);
                        if(!r) throw std::invalid_argument("unbound resource");
                        auto native=b.Uav?CD3DX12_RESOURCE_BARRIER::UAV(r):CD3DX12_RESOURCE_BARRIER::Transition(r,
                            static_cast<D3D12_RESOURCE_STATES>(b.Before),static_cast<D3D12_RESOURCE_STATES>(b.After));
                        l.Commands->ResourceBarrier(1,&native);
                    }
                };
                if(j==0) barriers(p.Before);
                if(p.Operation!=Invalid) record(p.Operation,j,l.Commands.Get());
                if(j+1==p.Lists) barriers(p.After);
                ThrowIfFailed(l.Commands->Close());
            } catch(...) { l.Commands->Close(); throw; }
            packets.back().push_back(l.Commands.Get());
        }
    }
    auto* q=_device.GetCommandQueue();
    try {
        if(prefix) { q->ExecuteCommandList(prefix); slot.Fence=prefix->FenceValue; }
        // U2 records sequentially and coalesces the ready packets into one
        // Execute batch. This also preserves explicit buffer states between
        // lists (D3D12 buffer decay happens at Execute boundaries).
        std::vector<ID3D12CommandList*> batch;
        for(auto& packet:packets) batch.insert(batch.end(),packet.begin(),packet.end());
        slot.Poisoned=true;
        q->GetCommandQueue()->ExecuteCommandLists(static_cast<UINT>(batch.size()),batch.data());
        const auto fence=q->Signal();
        slot.Fence=fence;
        for(auto& token:_tokens) token=fence;
        slot.Poisoned=false;
        if(suffix) { slot.Poisoned=true; q->ExecuteCommandList(suffix); slot.Fence=suffix->FenceValue; slot.Poisoned=false; }
        return slot.Fence;
    } catch(...) { slot.Poisoned=true; throw; }
}
}

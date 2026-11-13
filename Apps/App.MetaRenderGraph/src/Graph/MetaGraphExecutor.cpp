#include "MetaGraphExecutor.h"
#include "Engine.RendererDX12/GDX12CommandQueue.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include <stdexcept>
namespace Meta {
Executor::Executor(GDX12Device& d,uint32_t slots,uint32_t capacity,unsigned threads):_device(d),_jobs(threads),_slots(slots) {
    if(!slots || !capacity) throw std::invalid_argument("empty executor capacity");
    for(auto& slot:_slots) for(uint32_t i=0;i<capacity;++i) {
        List list;
        ThrowIfFailed(d.GetDevice()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&list.Allocator)));
        ThrowIfFailed(d.GetDevice()->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,list.Allocator.Get(),nullptr,IID_PPV_ARGS(&list.Commands)));
        ThrowIfFailed(list.Commands->Close()); slot.Lists.push_back(std::move(list));
    }
}
Executor::~Executor() {
    try { Drain(); } catch(...) {
        for(auto& s:_slots) for(auto& l:s.Lists) { l.Commands.Detach(); l.Allocator.Detach(); }
    }
}
void Executor::Owner() const { if(std::this_thread::get_id()!=_owner) throw std::logic_error("executor requires owner thread"); }
void Executor::Begin(uint32_t index) {
    Owner(); auto& slot=_slots.at(index);
    if(_aborted) throw std::logic_error("partial frame aborted; drain and recreate execution/resources");
    if(slot.Poisoned) throw std::logic_error("executor slot quarantined after submission failure");
    _device.GetCommandQueue()->CPUWaitForFenceValue(slot.Fence);
    _slot=index; _begun=true; _tokens.clear();
}
void Executor::Drain() {
    Owner(); _device.GetCommandQueue()->Flush();
    for(auto& s:_slots) if(s.Poisoned) throw std::logic_error("submission failed; executor quarantined");
}
uint64_t Executor::Execute(const Schedule& schedule,const std::vector<ID3D12Resource*>& resources,
    const Record& record,GDX12CommandList* prefix,GDX12CommandList* suffix,bool serial,
    const std::atomic<bool>* cancellation,std::ostream* trace) {
    Owner(); if(!_begun) throw std::logic_error("Begin required"); _begun=false;
    auto& slot=_slots[_slot]; _tokens.assign(schedule.Packets.size(),0);
    if(resources.size()!=schedule.Decays.size()) throw std::invalid_argument("resource binding count mismatch");
    for(size_t i=0;i<resources.size();++i) {
        if(!resources[i]) throw std::invalid_argument("unbound resource");
        D3D12_HEAP_PROPERTIES heap{}; D3D12_HEAP_FLAGS flags{};
        ThrowIfFailed(resources[i]->GetHeapProperties(&heap,&flags));
        const auto desc=resources[i]->GetDesc();
        if((desc.Flags&D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS)!=0 || heap.Type==D3D12_HEAP_TYPE_CUSTOM)
            throw std::invalid_argument("simultaneous-access textures/custom heaps require an explicit decay contract");
        const bool decays=desc.Dimension==D3D12_RESOURCE_DIMENSION_BUFFER && heap.Type==D3D12_HEAP_TYPE_DEFAULT;
        if(decays!=schedule.Decays[i]) throw std::invalid_argument("physical buffer decay contract mismatch");
    }
    size_t total=0; for(const auto& p:schedule.Packets) total+=size_t(p.Lists)+2;
    if(total>slot.Lists.size()) throw std::length_error("fixed command list capacity exceeded");
    for(Id i=0;i<schedule.Packets.size();++i)
        for(auto dep:schedule.Packets[i].Dependencies) if(dep>=i) throw std::invalid_argument("non-topological packet dependency");
    // Reset/freeze all ownership on the owner before starting any worker.
    // Every list has its own allocator; slot's last successful signal protects
    // allocators AND caller-owned descriptor/upload ranges through Begin.
    for(size_t i=0;i<total;++i) {
        ThrowIfFailed(slot.Lists[i].Allocator->Reset());
        ThrowIfFailed(slot.Lists[i].Commands->Reset(slot.Lists[i].Allocator.Get(),nullptr));
    }
    std::vector<std::vector<ID3D12CommandList*>> packets;
    std::vector<RecordingGroup> groups;
    std::vector<uint8_t> closed(total,0); // one distinct byte per recording job
    size_t cursor=0;
    for(Id i=0;i<schedule.Packets.size();++i) {
        const auto& p=schedule.Packets[i];
        RecordingGroup group{"packet "+std::to_string(i),{}, {}};
        RecordingGroup buckets{"buckets "+std::to_string(i),{}, {}};
        packets.emplace_back();
        for(uint32_t j=0;j<p.Lists+2;++j) {
            const auto index=cursor++;
            auto* cmd=slot.Lists[index].Commands.Get(); packets.back().push_back(cmd);
            RecordingGroup leaf{"list "+std::to_string(i)+":"+std::to_string(j),{},[&,i,j,cmd,index] {
                const auto& packet=schedule.Packets[i];
                try {
                    const auto barriers=[&](const std::vector<Barrier>& bs) {
                        for(auto b:bs) {
                            auto native=b.Uav?CD3DX12_RESOURCE_BARRIER::UAV(resources.at(b.Resource)):
                                CD3DX12_RESOURCE_BARRIER::Transition(resources.at(b.Resource),
                                    static_cast<D3D12_RESOURCE_STATES>(b.Before),static_cast<D3D12_RESOURCE_STATES>(b.After));
                            cmd->ResourceBarrier(1,&native);
                        }
                    };
                    if(j==0) barriers(packet.Before);
                    else if(j==packet.Lists+1) barriers(packet.After);
                    else if(packet.Operation!=Invalid) record(packet.Operation,j-1,cmd);
                    const auto closeResult=cmd->Close(); closed[index]=1;
                    ThrowIfFailed(closeResult);
                } catch(...) { if(!closed[index]) { cmd->Close(); closed[index]=1; } throw; }
            }};
            if(j==0 || j==p.Lists+1) group.Children.push_back(std::move(leaf));
            else buckets.Children.push_back(std::move(leaf));
        }
        group.Children.push_back(std::move(buckets)); groups.push_back(std::move(group));
    }
    Recorder recorder(_jobs,std::move(groups),serial,cancellation);
    auto* q=_device.GetCommandQueue();
    try {
        for(Id i=0;i<packets.size();++i) {
            recorder.Wait(i); // CPU-ready includes Close + nested continuation
            if(i==0 && prefix) { slot.Poisoned=true; q->ExecuteCommandList(prefix); slot.Fence=prefix->FenceValue; slot.Poisoned=false; }
            slot.Poisoned=true;
            q->GetCommandQueue()->ExecuteCommandLists(static_cast<UINT>(packets[i].size()),packets[i].data());
            const auto fence=q->Signal(); slot.Fence=fence; _tokens[i]=fence; slot.Poisoned=false;
            recorder.Mark("submit packet "+std::to_string(i));
        }
        if(suffix) { slot.Poisoned=true; q->ExecuteCommandList(suffix); slot.Fence=suffix->FenceValue; slot.Poisoned=false; }
        recorder.Finish(); if(trace) recorder.WriteTrace(*trace);
        return slot.Fence;
    } catch(...) {
        // Recording error does not quarantine already successfully signalled
        // work. Its last fence remains the slot retirement point. All workers
        // are joined before abandoning/resetting any unsubmitted list.
        recorder.Finish();
        for(auto token:_tokens) if(token) _aborted=true;
        for(size_t i=0;i<total;++i) if(!closed[i]) slot.Lists[i].Commands->Close();
        if(trace) recorder.WriteTrace(*trace);
        throw;
    }
}
}

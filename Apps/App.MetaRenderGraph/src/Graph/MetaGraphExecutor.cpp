#include "MetaGraphExecutor.h"
#include "Engine.RendererDX12/GDX12CommandQueue.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include <algorithm>
#include <limits>
#include <stdexcept>
namespace Meta {
namespace {
size_t QueueIndex(Queue queue) {
    switch(queue) {
    case Queue::Direct: return 0;
    case Queue::Compute: return 1;
    case Queue::Copy: return 2;
    }
    throw std::invalid_argument("unknown submission queue");
}
D3D12_COMMAND_LIST_TYPE ListType(Queue queue) {
    switch(queue) {
    case Queue::Direct: return D3D12_COMMAND_LIST_TYPE_DIRECT;
    case Queue::Compute: return D3D12_COMMAND_LIST_TYPE_COMPUTE;
    case Queue::Copy: return D3D12_COMMAND_LIST_TYPE_COPY;
    }
    throw std::invalid_argument("unknown command list queue");
}
D3D12_HEAP_TYPE HeapType(Heap heap) {
    switch(heap) {
    case Heap::Default: return D3D12_HEAP_TYPE_DEFAULT;
    case Heap::Upload: return D3D12_HEAP_TYPE_UPLOAD;
    case Heap::Readback: return D3D12_HEAP_TYPE_READBACK;
    }
    throw std::invalid_argument("unknown resource heap");
}
bool BarrierStateAllowed(Queue queue, State state) {
    const State copy=D3D12_RESOURCE_STATE_COPY_SOURCE | D3D12_RESOURCE_STATE_COPY_DEST;
    const State compute=copy | D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER |
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
        D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT | D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
    return queue==Queue::Direct || (state & ~(queue==Queue::Copy ? copy : compute))==0;
}
}
Executor::Executor(GDX12Device& d,uint32_t slots,uint32_t capacity,unsigned threads)
    :_device(d),_compute(std::make_unique<GDX12CommandQueue>(&d,D3D12_COMMAND_LIST_TYPE_COMPUTE)),
    _copy(std::make_unique<GDX12CommandQueue>(&d,D3D12_COMMAND_LIST_TYPE_COPY)),_jobs(threads),_slots(slots) {
    if(!slots || !capacity) throw std::invalid_argument("empty executor capacity");
    for(uint32_t i=0;i<slots;++i) {
        _slot=i; _begun=true; Reserve(capacity);
    }
    _slot=0; _begun=false;
}
Executor::~Executor() {
    try { Drain(); } catch(...) {
        for(auto& s:_slots) for(auto& lists:s.Lists) for(auto& l:lists) {
            l.Commands.Detach(); l.Allocator.Detach();
        }
        // Unknown completion must retain queues as well as their submitted lists.
        _compute.release(); _copy.release();
    }
}
void Executor::Owner() const { if(std::this_thread::get_id()!=_owner) throw std::logic_error("executor requires owner thread"); }
GDX12CommandQueue& Executor::SubmissionQueue(Queue queue) {
    switch(queue) {
    case Queue::Direct: return *_device.GetCommandQueue();
    case Queue::Compute: return *_compute;
    case Queue::Copy: return *_copy;
    }
    throw std::invalid_argument("unknown submission queue");
}
void Executor::Begin(uint32_t index) {
    Owner(); auto& slot=_slots.at(index);
    if(_aborted) throw std::logic_error("partial frame aborted; drain and recreate execution/resources");
    if(slot.Poisoned) throw std::logic_error("executor slot quarantined after submission failure");
    for(auto queue:{Queue::Direct,Queue::Compute,Queue::Copy})
        SubmissionQueue(queue).CPUWaitForFenceValue(slot.Fences[QueueIndex(queue)]);
    _slot=index; _begun=true; _tokens.clear();
}
void Executor::Drain() {
    Owner();
    for(auto& slot:_slots) for(auto queue:{Queue::Direct,Queue::Compute,Queue::Copy})
        SubmissionQueue(queue).CPUWaitForFenceValue(slot.Fences[QueueIndex(queue)]);
    for(auto& slot:_slots) if(slot.Poisoned) throw std::logic_error("submission failed; executor quarantined");
}
void Executor::Reserve(uint32_t count) {
    Owner(); if(!_begun) throw std::logic_error("Reserve requires a retired slot after Begin");
    for(auto queue:{Queue::Direct,Queue::Compute,Queue::Copy}) {
        auto& lists=_slots.at(_slot).Lists[QueueIndex(queue)];
        while(lists.size()<count) {
            List list;
            ThrowIfFailed(_device.GetDevice()->CreateCommandAllocator(ListType(queue),IID_PPV_ARGS(&list.Allocator)));
            ThrowIfFailed(_device.GetDevice()->CreateCommandList(0,ListType(queue),list.Allocator.Get(),nullptr,IID_PPV_ARGS(&list.Commands)));
            ThrowIfFailed(list.Commands->Close()); lists.push_back(std::move(list));
        }
    }
}
uint64_t Executor::Execute(const Schedule& schedule,const std::vector<ID3D12Resource*>& resources,
    const Record& record,GDX12CommandList* prefix,GDX12CommandList* suffix,bool serial,
    const std::atomic<bool>* cancellation,std::ostream* trace) {
    Owner(); if(!_begun) throw std::logic_error("Begin required"); _begun=false;
    auto& slot=_slots[_slot]; _tokens.assign(schedule.Packets.size(),0);
    if(resources.size()!=schedule.Decays.size() || resources.size()!=schedule.Memory.size())
        throw std::invalid_argument("resource binding count mismatch");
    for(size_t i=0;i<resources.size();++i) {
        if(!resources[i]) throw std::invalid_argument("unbound resource");
        D3D12_HEAP_PROPERTIES heap{}; D3D12_HEAP_FLAGS flags{};
        ThrowIfFailed(resources[i]->GetHeapProperties(&heap,&flags));
        const auto desc=resources[i]->GetDesc();
        if((desc.Flags&D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS)!=0 || heap.Type==D3D12_HEAP_TYPE_CUSTOM)
            throw std::invalid_argument("simultaneous-access textures/custom heaps require an explicit decay contract");
        if(heap.Type!=HeapType(schedule.Memory[i])) throw std::invalid_argument("physical resource heap contract mismatch");
        const bool decays=desc.Dimension==D3D12_RESOURCE_DIMENSION_BUFFER && heap.Type==D3D12_HEAP_TYPE_DEFAULT;
        if(decays!=schedule.Decays[i]) throw std::invalid_argument("physical buffer decay contract mismatch");
    }
    if(schedule.Packets.empty()) throw std::invalid_argument("missing terminal packet");
    const auto& terminal=schedule.Packets.back();
    if(terminal.Target!=Queue::Direct || terminal.Operation!=Invalid)
        throw std::invalid_argument("terminal packet must retire on DIRECT");
    for(Id i=0;i+1<schedule.Packets.size();++i)
        if(std::find(terminal.Dependencies.begin(),terminal.Dependencies.end(),i)==terminal.Dependencies.end())
            throw std::invalid_argument("terminal packet must join all prior packets");
    if((prefix && prefix->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT) ||
        (suffix && suffix->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT))
        throw std::invalid_argument("prefix/suffix must be DIRECT lists");
    std::array<size_t,3> counts{};
    for(Id i=0;i<schedule.Packets.size();++i) {
        const auto& p=schedule.Packets[i];
        if(!p.Lists || p.Lists>(std::numeric_limits<UINT>::max)()-2)
            throw std::invalid_argument("invalid packet list count");
        counts[QueueIndex(p.Target)]+=size_t(p.Lists)+2;
        for(auto dep:p.Dependencies) if(dep>=i) throw std::invalid_argument("non-topological packet dependency");
        const auto validateBarriers=[&](const std::vector<Barrier>& barriers) {
            for(const auto& b:barriers) {
                if(b.Resource>=resources.size() || schedule.Memory[b.Resource]!=Heap::Default)
                    throw std::invalid_argument("barrier requires a bound DEFAULT-heap resource");
                if(b.Uav ? p.Target==Queue::Copy :
                    !BarrierStateAllowed(p.Target,b.Before) || !BarrierStateAllowed(p.Target,b.After))
                    throw std::invalid_argument("barrier state is unsupported by packet queue");
            }
        };
        validateBarriers(p.Before); validateBarriers(p.After);
    }
    for(size_t q=0;q<counts.size();++q)
        if(counts[q]>slot.Lists[q].size()) throw std::length_error("fixed command list capacity exceeded");
    // Freeze list/allocator identity before any recording callback can run.
    std::vector<List*> lists;
    for(size_t q=0;q<counts.size();++q) for(size_t i=0;i<counts[q];++i) lists.push_back(&slot.Lists[q][i]);
    for(auto* list:lists) {
        ThrowIfFailed(list->Allocator->Reset());
        ThrowIfFailed(list->Commands->Reset(list->Allocator.Get(),nullptr));
    }
    std::vector<std::vector<ID3D12CommandList*>> packets;
    std::vector<RecordingGroup> groups;
    std::vector<std::vector<std::function<void()>>> ownerRecords(schedule.Packets.size());
    std::vector<uint8_t> closed(lists.size(),0);
    std::array<size_t,3> offsets{0,counts[0],counts[0]+counts[1]}, used{};
    for(Id i=0;i<schedule.Packets.size();++i) {
        const auto& p=schedule.Packets[i];
        RecordingGroup group{"packet "+std::to_string(i),{}, {}};
        RecordingGroup buckets{"buckets "+std::to_string(i),{}, {}};
        packets.emplace_back();
        const auto q=QueueIndex(p.Target);
        for(uint32_t j=0;j<p.Lists+2;++j) {
            const auto index=offsets[q]+used[q]++;
            auto* cmd=lists[index]->Commands.Get();
            if(cmd->GetType()!=ListType(p.Target)) throw std::logic_error("packet/list queue mismatch");
            packets.back().push_back(cmd);
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
            if(p.OwnerRecording && j>0 && j<=p.Lists) {
                ownerRecords[i].push_back(std::move(leaf.Continue));
                continue;
            }
            if(j==0 || j==p.Lists+1) group.Children.push_back(std::move(leaf));
            else buckets.Children.push_back(std::move(leaf));
        }
        group.Children.push_back(std::move(buckets)); groups.push_back(std::move(group));
    }
    Recorder recorder(_jobs,std::move(groups),serial,cancellation);
    auto& direct=SubmissionQueue(Queue::Direct);
    bool submitted=false;
    uint64_t prefixFence=0;
    std::array<bool,3> queueStarted{};
    try {
        for(Id i=0;i<packets.size();++i) {
            recorder.Wait(i);
            // Owner-only SDK sections no longer delay submission of earlier packets.
            for(auto& ownerRecord:ownerRecords[i]) {
                if(cancellation && cancellation->load()) throw std::runtime_error("owner recording cancelled");
                ownerRecord();
            }
            if(i==0 && prefix) {
                slot.Poisoned=true; submitted=true;
                direct.ExecuteCommandList(prefix);
                slot.Fences[0]=prefixFence=prefix->FenceValue; slot.Poisoned=false;
            }
            const auto target=schedule.Packets[i].Target;
            auto& q=SubmissionQueue(target);
            // Only published fences enter native waits; failed recording cannot
            // strand another queue behind a token that will never be signalled.
            if(target!=Queue::Direct && !queueStarted[QueueIndex(target)]) {
                // Persistent resources can be shared across slots; their prior
                // frame's DIRECT terminal joins every queue before a new writer.
                if(_previousCompletion) q.WaitForQueue(direct,_previousCompletion);
                if(prefixFence) q.WaitForQueue(direct,prefixFence);
                queueStarted[QueueIndex(target)]=true;
            }
            for(auto dep:schedule.Packets[i].Dependencies) {
                if(!_tokens[dep]) throw std::logic_error("dependency has no submitted completion token");
                const auto producer=schedule.Packets[dep].Target;
                if(producer!=target) q.WaitForQueue(SubmissionQueue(producer),_tokens[dep]);
            }
            slot.Poisoned=true; submitted=true;
            q.GetCommandQueue()->ExecuteCommandLists(static_cast<UINT>(packets[i].size()),packets[i].data());
            const auto fence=q.Signal(); slot.Fences[QueueIndex(target)]=fence; _tokens[i]=fence; slot.Poisoned=false;
            recorder.Mark("submit packet "+std::to_string(i));
        }
        if(suffix) {
            slot.Poisoned=true; submitted=true;
            direct.ExecuteCommandList(suffix); slot.Fences[0]=suffix->FenceValue; slot.Poisoned=false;
        }
        recorder.Finish(); if(trace) recorder.WriteTrace(*trace);
        return _previousCompletion=slot.Fences[0];
    } catch(...) {
        recorder.Finish();
        if(submitted) _aborted=true;
        for(size_t i=0;i<lists.size();++i) if(!closed[i]) lists[i]->Commands->Close();
        if(trace) recorder.WriteTrace(*trace);
        throw;
    }
}
}

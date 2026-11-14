#include "CrossAdapterTransfer.h"
#include "Engine.Core/Logging/Logger.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <stdexcept>
#include <thread>
#pragma comment(lib,"d3dcompiler.lib")
namespace Meta {
namespace {
constexpr uint32_t Slots=3, Words=256, HeaderWords=8;
constexpr uint64_t Bytes=uint64_t(Words+HeaderWords)*sizeof(uint32_t);
struct Publication {
    uint64_t Sequence=0, Generation=0, Incarnation=0;
    uint32_t PayloadBytes=uint32_t(Bytes), Seed=0;
};
static_assert(sizeof(Publication)==32,"publication header must match deterministic shader");
static_assert(Words==256 && HeaderWords==8,"update the embedded producer shader with its frozen layout");
ComPtr<ID3D12Resource> Buffer(GDX12Device& device,D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE) {
    ComPtr<ID3D12Resource> resource;
    const CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
    const auto desc=CD3DX12_RESOURCE_DESC::Buffer(Bytes,flags);
    ThrowIfFailed(device.GetDevice()->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,
        D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&resource)));
    return resource;
}
template<class T> ComPtr<T> Open(GDX12Device& owner,T* object,GDX12Device& other) {
    HANDLE handle=nullptr;
    ThrowIfFailed(owner.GetDevice()->CreateSharedHandle(object,nullptr,GENERIC_ALL,nullptr,&handle));
    ComPtr<T> imported;
    const auto hr=other.GetDevice()->OpenSharedHandle(handle,IID_PPV_ARGS(&imported));
    CloseHandle(handle); ThrowIfFailed(hr); return imported;
}
Id PacketFor(const Schedule& schedule,Id operation) {
    for(Id i=0;i<schedule.Packets.size();++i) if(schedule.Packets[i].Operation==operation) return i;
    throw std::logic_error("publication operation missing from compiled plan");
}
// Both header and payload are generated in one dispatch and copied as one buffer.
// No test/readback path or legacy offload worker is invoked by this fixture.
constexpr char ProducerShader[]=R"(
cbuffer Inputs : register(b0) {
    uint seqLo, seqHi, genLo, genHi, epochLo, epochHi, bytes, seed;
};
RWByteAddressBuffer Packed : register(u0);
[numthreads(64,1,1)] void Produce(uint3 tid : SV_DispatchThreadID) {
    uint i=tid.x;
    if(i==0) {
        Packed.Store4(0,uint4(seqLo,seqHi,genLo,genHi));
        Packed.Store4(16,uint4(epochLo,epochHi,bytes,seed));
    }
    if(i<256) Packed.Store(32+4*i,(seed+i*1664525u)^seqLo^(seqHi*1013904223u));
})";
}
struct CrossAdapterTransfer::Impl {
    struct PackedSlot { ComPtr<ID3D12Resource> Resource; CompletionToken Retired; };
    struct SharedSlot {
        ComPtr<ID3D12Heap> SecondaryHeap, PrimaryHeap;
        ComPtr<ID3D12Resource> Secondary, Primary;
        Publication Metadata;
        CompletionToken Ready, Imported;
        uint64_t Generation=0;
        bool Occupied=false, Writing=false, Claimed=false;
    };
    struct DisplaySlot {
        ComPtr<ID3D12Resource> Resource;
        Publication Metadata;
        CompletionToken Imported;
        std::array<CompletionToken,3> Readers{};
        uint32_t Pins=0;
    };
    GDX12Device& Primary;
    GDX12Device Secondary;
    std::shared_ptr<QueueSubmission> Submit;
    std::unique_ptr<Executor> Import;
    std::array<PackedSlot,Slots> Packed;
    std::array<SharedSlot,Slots> Shared;
    std::array<DisplaySlot,Slots> Display;
    std::vector<std::array<ComPtr<ID3D12Resource>,2>> Consumers;
    ComPtr<ID3D12RootSignature> Root;
    ComPtr<ID3D12PipelineState> Pipeline;
    mutable std::mutex Mutex;
    std::condition_variable Wake;
    std::thread Worker;
    std::atomic<bool> Cancel{false};
    std::exception_ptr Failure;
    uint32_t Seed=1;
    uint64_t Steps=0, LastDisplayed=0;
    Id Current=Invalid, Pending=Invalid, FrameDisplay=Invalid, LastSubmittedDisplay=Invalid;
    uint32_t FrameSlot=0;
    std::array<Id,2> Readers{Invalid,Invalid};
    std::thread::id Owner=std::this_thread::get_id();
    bool Started=false, Stopped=false;

    Impl(GDX12Device& primary,int adapter,uint32_t frames):Primary(primary),Consumers(frames) {
        if(adapter<0 || !frames) throw std::invalid_argument("explicit secondary adapter and frame slots required");
        ComPtr<IDXGIFactory6> factory; ThrowIfFailed(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)));
        ComPtr<IDXGIAdapter1> primaryAdapter,secondaryAdapter;
        // Resolve primary through its actual LUID, never through a guessed index.
        ThrowIfFailed(factory->EnumAdapterByLuid(Primary.GetAdapterLuid(),IID_PPV_ARGS(&primaryAdapter)));
        ThrowIfFailed(factory->EnumAdapters1(UINT(adapter),&secondaryAdapter));
        DXGI_ADAPTER_DESC1 a{},b{};
        ThrowIfFailed(primaryAdapter->GetDesc1(&a)); ThrowIfFailed(secondaryAdapter->GetDesc1(&b));
        if((a.Flags|b.Flags)&DXGI_ADAPTER_FLAG_SOFTWARE ||
            (a.AdapterLuid.HighPart==b.AdapterLuid.HighPart && a.AdapterLuid.LowPart==b.AdapterLuid.LowPart))
            throw std::invalid_argument("Meta.CrossAdapter requires two different physical hardware adapters");
        ComPtr<IDXGIAdapter4> selected; ThrowIfFailed(secondaryAdapter.As(&selected));
        ThrowIfFailed(Secondary.Initialize(selected));
        Submit=std::make_shared<QueueSubmission>(Primary,&Secondary);
        Import=std::make_unique<Executor>(Submit,0,Slots,6,2);
        for(auto& p:Packed) p.Resource=Buffer(Secondary,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        for(auto& d:Display) d.Resource=Buffer(Primary);
        for(auto& frame:Consumers) for(auto& result:frame) result=Buffer(Primary);
        const auto desc=CD3DX12_RESOURCE_DESC::Buffer(Bytes,D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER);
        const auto x=Secondary.GetDevice()->GetResourceAllocationInfo(0,1,&desc);
        const auto y=Primary.GetDevice()->GetResourceAllocationInfo(0,1,&desc);
        if(x.SizeInBytes==UINT64_MAX || y.SizeInBytes==UINT64_MAX)
            throw std::runtime_error("cross-adapter buffer allocation unsupported");
        D3D12_HEAP_DESC heap{};
        heap.SizeInBytes=(std::max)(x.SizeInBytes,y.SizeInBytes);
        heap.Alignment=(std::max)(x.Alignment,y.Alignment);
        heap.Properties=CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        heap.Flags=D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER;
        for(auto& s:Shared) {
            ThrowIfFailed(Secondary.GetDevice()->CreateHeap(&heap,IID_PPV_ARGS(&s.SecondaryHeap)));
            s.PrimaryHeap=Open(Secondary,s.SecondaryHeap.Get(),Primary);
            ThrowIfFailed(Secondary.GetDevice()->CreatePlacedResource(s.SecondaryHeap.Get(),0,&desc,
                D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&s.Secondary)));
            ThrowIfFailed(Primary.GetDevice()->CreatePlacedResource(s.PrimaryHeap.Get(),0,&desc,
                D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&s.Primary)));
            Submit->ValidateResource(0,s.Primary.Get()); Submit->ValidateResource(1,s.Secondary.Get());
        }
        D3D12_ROOT_PARAMETER parameters[2]{};
        parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[0].Constants.Num32BitValues=HeaderWords;
        parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;
        D3D12_ROOT_SIGNATURE_DESC root{}; root.NumParameters=2; root.pParameters=parameters;
        ComPtr<ID3DBlob> code,errors,signature;
        ThrowIfFailed(D3D12SerializeRootSignature(&root,D3D_ROOT_SIGNATURE_VERSION_1,&signature,&errors));
        ThrowIfFailed(Secondary.GetDevice()->CreateRootSignature(0,signature->GetBufferPointer(),signature->GetBufferSize(),IID_PPV_ARGS(&Root)));
        ThrowIfFailed(D3DCompile(ProducerShader,sizeof(ProducerShader)-1,"Meta deterministic producer",nullptr,nullptr,
            "Produce","cs_5_1",D3DCOMPILE_ENABLE_STRICTNESS,0,&code,&errors));
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature=Root.Get(); pso.CS={code->GetBufferPointer(),code->GetBufferSize()};
        ThrowIfFailed(Secondary.GetDevice()->CreateComputePipelineState(&pso,IID_PPV_ARGS(&Pipeline)));
        Log::Info("MetaRenderGraph","CrossAdapter primary LUID=",a.AdapterLuid.HighPart,":",a.AdapterLuid.LowPart,
            " secondary index=",adapter," LUID=",b.AdapterLuid.HighPart,":",b.AdapterLuid.LowPart,
            " incarnation=",Submit->Incarnation()," payload bytes=",Bytes," packed/shared/display slots=",Slots);
    }
    void CheckOwner() const {
        if(Owner!=std::this_thread::get_id()) throw std::logic_error("cross-adapter host requires primary owner thread");
    }
    void CheckMetadata(const Publication& m) const {
        if(!m.Sequence || !m.Generation || m.Incarnation!=Submit->Incarnation() || m.PayloadBytes!=Bytes)
            throw std::invalid_argument("invalid publication bounds/generation/device incarnation");
    }
    bool FreeDisplay(Id i) const {
        const auto& d=Display[i];
        if(i==Current || i==Pending || i==LastSubmittedDisplay || d.Pins || (d.Imported && !Submit->Complete(d.Imported))) return false;
        for(auto reader:d.Readers) if(reader && !Submit->Complete(reader)) return false;
        return true;
    }
    void Produce() {
        try {
            // Construct on its owner thread: separate allocators, JobSystem and
            // immutable captures can record concurrently with the primary scene.
            Executor context(Submit,1,Slots,6,2);
            while(!Cancel.load()) {
                Id packed=Invalid,shared=Invalid;
                Publication input;
                {
                    std::unique_lock<std::mutex> lock(Mutex);
                    for(Id i=0;i<Slots;++i) {
                        auto& s=Shared[i];
                        if(s.Occupied && s.Claimed && s.Imported && Submit->Complete(s.Imported)) s.Occupied=false;
                        if(shared==Invalid && !s.Occupied && !s.Writing) shared=i;
                        if(packed==Invalid && (!Packed[i].Retired || Submit->Complete(Packed[i].Retired)) && context.TryBegin(i)) packed=i;
                    }
                    if(packed==Invalid) { Wake.wait_for(lock,std::chrono::milliseconds(1)); continue; }
                    if(Steps==UINT64_MAX) throw std::overflow_error("producer sequence exhausted");
                    input.Sequence=Steps+1; input.Incarnation=Submit->Incarnation(); input.Seed=Seed;
                    if(shared!=Invalid) {
                        auto& s=Shared[shared];
                        if(s.Generation==UINT64_MAX) throw std::overflow_error("publication generation exhausted");
                        input.Generation=++s.Generation; s.Writing=true;
                    }
                }
                Graph graph; graph.Device=1; graph.FrameBoundary=false; graph.TerminalQueue=Queue::Copy;
                graph.Resources={{"secondary packed",0,0,true,true}};
                graph.Operations={{"deterministic produce/pack",{{0,Mode::Write,UnorderedAccess}},{},false,1,false,Queue::Compute}};
                std::vector<ID3D12Resource*> resources{Packed[packed].Resource.Get()};
                if(shared!=Invalid) {
                    graph.Resources.push_back({"secondary shared handle",0,0,false,true});
                    graph.Operations.push_back({"export publication",{{0,Mode::Read,D3D12_RESOURCE_STATE_COPY_SOURCE},
                        {1,Mode::Write,D3D12_RESOURCE_STATE_COPY_DEST}},{0},false,1,false,Queue::Copy});
                    resources.push_back(Shared[shared].Secondary.Get());
                }
                const auto schedule=Compile(graph);
                context.Execute(schedule,resources,[&,input,packed,shared](Id op,uint32_t,ID3D12GraphicsCommandList* cmd) {
                    if(op==0) {
                        cmd->SetPipelineState(Pipeline.Get()); cmd->SetComputeRootSignature(Root.Get());
                        cmd->SetComputeRoot32BitConstants(0,HeaderWords,&input,0);
                        cmd->SetComputeRootUnorderedAccessView(1,Packed[packed].Resource->GetGPUVirtualAddress());
                        cmd->Dispatch((Words+63)/64,1,1);
                    } else {
                        CheckMetadata(input);
                        cmd->CopyBufferRegion(Shared[shared].Secondary.Get(),0,Packed[packed].Resource.Get(),0,Bytes);
                    }
                },nullptr,nullptr,false,&Cancel);
                const auto retired=context.Completions().at(PacketFor(schedule,shared==Invalid ? 0:1));
                {
                    std::lock_guard<std::mutex> lock(Mutex);
                    Packed[packed].Retired=retired; Steps=input.Sequence;
                    if(shared!=Invalid) {
                        auto& s=Shared[shared];
                        // Atomic CPU publication AFTER the export signal was issued.
                        s.Metadata=input; s.Ready=retired; s.Imported={};
                        s.Claimed=false; s.Occupied=true; s.Writing=false;
                    }
                }
            }
            context.Drain();
        } catch(...) {
            std::lock_guard<std::mutex> lock(Mutex);
            if(!Cancel.load()) Failure=std::current_exception();
            Cancel.store(true); Wake.notify_all();
        }
    }
    void Select(PublicationSelection mode,uint64_t required) {
        if(Pending!=Invalid && Submit->Complete(Display[Pending].Imported)) { Current=Pending; Pending=Invalid; }
        if(Pending==Invalid) {
            Id display=Invalid,source=Invalid;
            for(Id i=0;i<Slots;++i) if(FreeDisplay(i)) { display=i; break; }
            const uint64_t previous=Current==Invalid ? 0:Display[Current].Metadata.Sequence;
            if(display!=Invalid) for(Id i=0;i<Slots;++i) {
                auto& s=Shared[i];
                if(!s.Occupied || s.Claimed || s.Writing || s.Metadata.Sequence<=previous ||
                    (required && s.Metadata.Sequence!=required)) continue;
                CheckMetadata(s.Metadata);
                if(mode==PublicationSelection::LatestCompleted && !Submit->Complete(s.Ready)) continue;
                if(source==Invalid || s.Metadata.Sequence>Shared[source].Metadata.Sequence) source=i;
            }
            if(source!=Invalid && Import->TryBegin(display)) {
                auto& s=Shared[source]; auto& d=Display[display];
                if(s.Metadata.Generation!=s.Generation) throw std::logic_error("shared publication generation mismatch");
                Graph graph; graph.FrameBoundary=false; graph.TerminalQueue=Queue::Copy;
                graph.Resources={{"primary shared handle",0,0,false,true},{"primary display",0,0,false,true}};
                graph.Operations={{"import publication",{{0,Mode::Read,D3D12_RESOURCE_STATE_COPY_SOURCE},
                    {1,Mode::Write,D3D12_RESOURCE_STATE_COPY_DEST}},{},false,1,false,Queue::Copy}};
                const auto schedule=Compile(graph);
                s.Claimed=true;
                Import->Execute(schedule,{s.Primary.Get(),d.Resource.Get()},[&](Id,uint32_t,ID3D12GraphicsCommandList* cmd) {
                    CheckMetadata(s.Metadata);
                    cmd->CopyBufferRegion(d.Resource.Get(),0,s.Primary.Get(),0,s.Metadata.PayloadBytes);
                },nullptr,nullptr,false,nullptr,nullptr,{{PacketFor(schedule,0),s.Ready}});
                d.Metadata=s.Metadata; d.Imported=s.Imported=Import->Completions().at(PacketFor(schedule,0));
                d.Readers={}; Pending=display;
                // Older, unclaimed completed exports have no readers. Drop them
                // explicitly so LatestCompleted cannot strand a full ring forever.
                for(auto& old:Shared) if(old.Occupied && !old.Claimed && !old.Writing &&
                    old.Metadata.Sequence<s.Metadata.Sequence && Submit->Complete(old.Ready)) old.Occupied=false;
            }
        }
        FrameDisplay=mode==PublicationSelection::StrictPublication && Pending!=Invalid ? Pending:Current;
        if(mode==PublicationSelection::LatestCompleted && LastSubmittedDisplay!=Invalid &&
            (FrameDisplay==Invalid || Display[FrameDisplay].Metadata.Sequence<LastDisplayed))
            FrameDisplay=LastSubmittedDisplay;
        if(required) {
            FrameDisplay=Invalid;
            for(Id i:{Pending,Current}) if(i!=Invalid && Display[i].Metadata.Sequence==required) FrameDisplay=i;
        }
        if(FrameDisplay!=Invalid) { CheckMetadata(Display[FrameDisplay].Metadata); ++Display[FrameDisplay].Pins; }
    }
};
CrossAdapterTransfer::CrossAdapterTransfer(GDX12Device& primary,int adapter,uint32_t frames)
    :_impl(std::make_unique<Impl>(primary,adapter,frames)) {}
CrossAdapterTransfer::~CrossAdapterTransfer() {
    try { Stop(); } catch(...) {
        _impl->Cancel.store(true); _impl->Wake.notify_all();
        if(_impl->Worker.joinable()) _impl->Worker.join();
        OutputDebugStringA("Meta cross-adapter drain failed; retaining devices, queues, slots and recording contexts\n");
        _impl.release(); // quarantine retains ALL resources and both local handles
    }
}
const std::shared_ptr<QueueSubmission>& CrossAdapterTransfer::Submissions() const { return _impl->Submit; }
void CrossAdapterTransfer::Start() {
    auto& p=*_impl; p.CheckOwner();
    if(p.Started && !p.Stopped) throw std::logic_error("producer already running");
    if(p.Failure) std::rethrow_exception(p.Failure);
    p.Submit->CheckHealth(); p.Cancel.store(false); p.Stopped=false;
    p.Worker=std::thread([&p] { p.Produce(); }); p.Started=true;
}
void CrossAdapterTransfer::Stop() {
    auto& p=*_impl; p.CheckOwner();
    p.Cancel.store(true); p.Wake.notify_all();
    if(p.Worker.joinable()) p.Worker.join();
    p.Submit->CheckHealth();
    p.Import->Drain();
    for(const auto& packed:p.Packed) if(packed.Retired) p.Submit->Wait(packed.Retired);
    for(const auto& shared:p.Shared) if(shared.Ready) p.Submit->Wait(shared.Ready);
    for(const auto& d:p.Display) {
        if(d.Imported) p.Submit->Wait(d.Imported);
        for(auto reader:d.Readers) if(reader) p.Submit->Wait(reader);
        if(d.Pins) throw std::logic_error("display lease has no complete reader retirement evidence; quarantine required");
    }
    // Cancellation may stop recording after reserving a slot but before its
    // export was issued. The joined producer context has now drained that work.
    for(auto& shared:p.Shared) if(shared.Writing) {
        shared.Writing=false; shared.Occupied=false; shared.Claimed=false;
        shared.Ready={}; shared.Imported={}; shared.Metadata={};
    }
    p.Stopped=true;
}
void CrossAdapterTransfer::SetSeed(uint32_t seed) {
    std::lock_guard<std::mutex> lock(_impl->Mutex); _impl->Seed=seed;
}
uint64_t CrossAdapterTransfer::Append(Graph& graph,std::vector<ID3D12Resource*>& bindings,uint32_t slot,
    PublicationSelection mode,uint64_t required) {
    auto& p=*_impl; p.CheckOwner();
    if(p.Stopped) Start(); // world drain pauses producer; ordinary Present never ticks it
    std::lock_guard<std::mutex> lock(p.Mutex);
    if(p.Failure) std::rethrow_exception(p.Failure);
    if(mode!=PublicationSelection::StrictPublication && mode!=PublicationSelection::LatestCompleted)
        throw std::invalid_argument("unknown publication selection");
    if(required && mode!=PublicationSelection::StrictPublication)
        throw std::invalid_argument("an exact publication requires StrictPublication");
    if(!p.Started || p.Stopped || p.FrameDisplay!=Invalid) throw std::logic_error("invalid publication host lifecycle");
    if(graph.Device!=0 || bindings.size()!=graph.Resources.size()) throw std::invalid_argument("invalid primary graph binding");
    p.Consumers.at(slot); p.FrameSlot=slot; p.Readers={Invalid,Invalid};
    p.Select(mode,required);
    if(p.FrameDisplay==Invalid) return 0; // no fake initial publication
    const Id display=Id(graph.Resources.size());
    graph.Resources.push_back({"cross-adapter selected display",0,0,false,true});
    bindings.push_back(p.Display[p.FrameDisplay].Resource.Get());
    for(Id reader=0;reader<2;++reader) {
        const Id result=Id(graph.Resources.size());
        graph.Resources.push_back({"cross-adapter DIRECT reader result",0,0,false,true});
        bindings.push_back(p.Consumers[slot][reader].Get());
        p.Readers[reader]=Id(graph.Operations.size());
        graph.Operations.push_back({"cross-adapter DIRECT reader "+std::to_string(reader),
            {{display,Mode::Read,D3D12_RESOURCE_STATE_COPY_SOURCE},{result,Mode::Write,D3D12_RESOURCE_STATE_COPY_DEST}},
            reader ? std::vector<Id>{p.Readers[0]}:std::vector<Id>{},false,1,false,Queue::Direct});
    }
    return p.Display[p.FrameDisplay].Metadata.Sequence;
}
bool CrossAdapterTransfer::Record(Id operation,ID3D12GraphicsCommandList* cmd) {
    auto& p=*_impl;
    for(Id reader=0;reader<2;++reader) if(operation==p.Readers[reader] && operation!=Invalid) {
        const auto& d=p.Display.at(p.FrameDisplay); p.CheckMetadata(d.Metadata);
        cmd->CopyBufferRegion(p.Consumers[p.FrameSlot][reader].Get(),0,d.Resource.Get(),0,d.Metadata.PayloadBytes);
        return true;
    }
    return false;
}
std::vector<PacketWait> CrossAdapterTransfer::Waits(const Schedule& schedule) const {
    const auto& p=*_impl;
    if(p.FrameDisplay==Invalid) return {};
    // Prior DIRECT readers already queued the import wait for this display.
    if(p.FrameDisplay==p.LastSubmittedDisplay) return {};
    return {{PacketFor(schedule,p.Readers[0]),p.Display[p.FrameDisplay].Imported}};
}
void CrossAdapterTransfer::RetireFrame(const Schedule& schedule,const std::vector<CompletionToken>& tokens,bool success) {
    auto& p=*_impl; p.CheckOwner();
    if(p.FrameDisplay==Invalid) return;
    auto& d=p.Display[p.FrameDisplay];
    if(tokens.size()!=schedule.Packets.size()) throw std::logic_error("missing reader retirement evidence");
    for(auto reader:p.Readers) {
        const auto token=tokens.at(PacketFor(schedule,reader));
        if(!token) {
            if(success) throw std::logic_error("successful frame missing reader token");
            continue; // failed/cancelled recording never issued this reader
        }
        if(token.Device!=0 || token.Target!=Queue::Direct) throw std::logic_error("foreign display reader token");
        p.Submit->Complete(token); // validate issued device incarnation, no CPU wait
        auto& last=d.Readers[0]; if(token.Value>last.Value) last=token;
    }
    if(!d.Pins) throw std::logic_error("display lease underflow");
    if(success) { p.LastDisplayed=d.Metadata.Sequence; p.LastSubmittedDisplay=p.FrameDisplay; }
    --d.Pins; p.FrameDisplay=Invalid; p.Readers={Invalid,Invalid}; p.Wake.notify_all();
}
uint64_t CrossAdapterTransfer::SubmittedSteps() const {
    std::lock_guard<std::mutex> lock(_impl->Mutex); return _impl->Steps;
}
uint64_t CrossAdapterTransfer::DisplayedSequence() const {
    const auto& p=*_impl; p.CheckOwner();
    return p.LastDisplayed;
}
ID3D12Resource* CrossAdapterTransfer::ConsumerResult(uint32_t slot,uint32_t reader) const {
    return _impl->Consumers.at(slot).at(reader).Get();
}
}

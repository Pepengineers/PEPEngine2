#include "QueueSubmission.h"
#include "Engine.RendererDX12/GDX12CommandQueue.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include <atomic>
#include <stdexcept>
#include <thread>
#include <chrono>
#include <climits>
namespace Meta {
namespace {
std::atomic<uint64_t> NextIncarnation{1};
size_t Index(Queue queue) {
    switch(queue) { case Queue::Direct:return 0; case Queue::Compute:return 1; case Queue::Copy:return 2; }
    throw std::invalid_argument("unknown queue identity");
}
bool Equal(LUID a,LUID b) { return a.HighPart==b.HighPart && a.LowPart==b.LowPart; }
}
QueueSubmission::QueueSubmission(GDX12Device& primary,GDX12Device* secondary)
    : _devices{&primary,secondary},_incarnation(NextIncarnation.fetch_add(1)) {
    if(secondary && Equal(primary.GetAdapterLuid(),secondary->GetAdapterLuid()))
        throw std::invalid_argument("two distinct physical adapter LUIDs required");
    for(Id d=0;d<2;++d) if(_devices[d]) _identities[d]=_devices[d]->GetDevice();
    for(Id d=0;d<2;++d) if(_devices[d]) for(auto q:{Queue::Direct,Queue::Compute,Queue::Copy}) {
        auto& t=_queues[d][Index(q)];
        if(q==Queue::Direct) t.Queue=_devices[d]->GetCommandQueue();
        else {
            t.Owned=std::make_unique<GDX12CommandQueue>(_devices[d],q==Queue::Copy ?
                D3D12_COMMAND_LIST_TYPE_COPY : D3D12_COMMAND_LIST_TYPE_COMPUTE);
            t.Queue=t.Owned.get();
        }
        if(secondary) {
            ThrowIfFailed(Device(d).GetDevice()->CreateFence(0,
                D3D12_FENCE_FLAG_SHARED | D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER,IID_PPV_ARGS(&t.Shared)));
            HANDLE handle=nullptr;
            ThrowIfFailed(Device(d).GetDevice()->CreateSharedHandle(t.Shared.Get(),nullptr,GENERIC_ALL,nullptr,&handle));
            const auto hr=Device(1-d).GetDevice()->OpenSharedHandle(handle,IID_PPV_ARGS(&t.Remote));
            CloseHandle(handle); ThrowIfFailed(hr);
        }
    }
}
QueueSubmission::~QueueSubmission() {
    // A failed Signal leaves unknown in-flight work. Retain queue, device and
    // cross-device fence handles rather than release objects under that work.
    try { Check(); } catch(...) { _quarantined.store(true); }
    if(!_quarantined.load()) return;
    // Any removed device can strand queues on the other device. Quarantine the
    // entire domain, including healthy-looking queues behind shared waits.
    for(auto& identity:_identities) identity.Detach();
    for(auto& device:_queues) for(auto& t:device) {
        t.Owned.release(); t.Shared.Detach(); t.Remote.Detach();
        if(t.Queue) t.Queue->GetCommandQueue()->AddRef();
    }
}
GDX12Device& QueueSubmission::Device(Id id) const {
    if(id>1 || !_devices[id]) throw std::invalid_argument("unconfigured device context");
    return *_devices[id];
}
void QueueSubmission::Check() const {
    if(_quarantined.load()) throw std::runtime_error("submission domain quarantined");
    try {
        for(Id d=0;d<2;++d) if(_devices[d]) {
            if(_devices[d]->GetDevice().Get()!=_identities[d].Get())
                throw std::runtime_error("device context incarnation changed");
            ThrowIfFailed(_identities[d]->GetDeviceRemovedReason());
        }
        for(const auto& device:_queues) for(const auto& t:device)
            if(t.Poisoned) throw std::runtime_error("submission domain quarantined after native failure");
    } catch(...) { _quarantined.store(true); throw; }
}
void QueueSubmission::CheckHealth() const { std::lock_guard<std::mutex> lock(_mutex); Check(); }
const QueueSubmission::Timeline& QueueSubmission::Resolve(CompletionToken token) const {
    Device(token.Device);
    const auto& t=_queues[token.Device][Index(token.Target)];
    if(!token || token.Incarnation!=_incarnation || token.Value>t.Issued)
        throw std::invalid_argument("stale or unissued completion token");
    return t;
}
bool QueueSubmission::Complete(CompletionToken token) const {
    std::lock_guard<std::mutex> lock(_mutex); Check();
    const auto value=Resolve(token).Queue->GetFence()->GetCompletedValue();
    if(value==UINT64_MAX) { _quarantined.store(true); ThrowIfFailed(DXGI_ERROR_DEVICE_REMOVED); }
    return value>=token.Value;
}
void QueueSubmission::Wait(CompletionToken token) const {
    // Polling also observes removal of the OTHER device behind a shared wait.
    while(!Complete(token)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
}
void QueueSubmission::ValidateResource(Id id,ID3D12Resource* resource) const {
    if(!resource) throw std::invalid_argument("unbound resource");
    ComPtr<ID3D12Device> owner;
    ThrowIfFailed(resource->GetDevice(IID_PPV_ARGS(&owner)));
    ComPtr<IUnknown> a,b;
    ThrowIfFailed(owner.As(&a)); ThrowIfFailed(Device(id).GetDevice().As(&b));
    if(a.Get()!=b.Get() || !Equal(owner->GetAdapterLuid(),Device(id).GetAdapterLuid()))
        throw std::invalid_argument("resource bound to wrong physical device/incarnation");
}
CompletionToken QueueSubmission::Submit(Id device,Meta::Queue target,
    const std::vector<ID3D12CommandList*>& lists,const std::vector<CompletionToken>& waits,GDX12CommandList* external) {
    std::lock_guard<std::mutex> lock(_mutex); Check();
    Device(device); auto& t=_queues.at(device).at(Index(target));
    if((lists.empty() && !external) || lists.size()>UINT_MAX)
        throw std::invalid_argument("empty/oversized packet submission");
    for(auto* list:lists) {
        ComPtr<ID3D12Device> owner; ThrowIfFailed(list->GetDevice(IID_PPV_ARGS(&owner)));
        ComPtr<IUnknown> a,b;
        ThrowIfFailed(owner.As(&a)); ThrowIfFailed(Device(device).GetDevice().As(&b));
        if(a.Get()!=b.Get()) throw std::invalid_argument("foreign command list device");
    }
    for(auto token:waits) Resolve(token); // validate EVERY token before native mutation
    try {
        t.Poisoned=true;
        for(auto token:waits) {
            const auto& producer=Resolve(token);
            if(token.Device==device && token.Target==target) continue;
            auto* fence=token.Device==device ? producer.Queue->GetFence().Get() : producer.Remote.Get();
            if(!fence) throw std::invalid_argument("missing cross-device fence handle");
            ThrowIfFailed(t.Queue->GetCommandQueue()->Wait(fence,token.Value));
        }
        uint64_t value=0;
        if(external) { t.Queue->ExecuteCommandList(external); value=external->FenceValue; }
        else {
            t.Queue->GetCommandQueue()->ExecuteCommandLists(UINT(lists.size()),lists.data());
            value=t.Queue->Signal();
        }
        if(t.Shared) ThrowIfFailed(t.Queue->GetCommandQueue()->Signal(t.Shared.Get(),value));
        t.Issued=value; t.Poisoned=false;
        return {device,target,value,_incarnation};
    } catch(...) { _quarantined.store(true); throw; } // No token is published.
}
}

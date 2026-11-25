#include "QueueSubmission.h"
#include "Engine.RendererDX12/GDX12CommandQueue.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.Core/Logging/Logger.h"
#include <algorithm>
#include <atomic>
#include <stdexcept>
#include <thread>
#include <chrono>
#include <climits>
#include <cassert>
#include <filesystem>
namespace BistroRender {
namespace {
std::atomic<uint64_t> NextIncarnation{1};
size_t Index(Queue queue) {
    switch(queue) { case Queue::Direct:return 0; case Queue::Compute:return 1; case Queue::Copy:return 2; }
    throw std::invalid_argument("unknown queue identity");
}
bool Equal(LUID a,LUID b) { return a.HighPart==b.HighPart && a.LowPart==b.LowPart; }
}
namespace {
ComPtr<ID3D12Fence> CreateSharedFence(ID3D12Device& device)
{
    ComPtr<ID3D12Fence> fence;
    ThrowIfFailed(device.CreateFence(0, D3D12_FENCE_FLAG_SHARED | D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER,
                                     IID_PPV_ARGS(&fence)));
    return fence;
}

ComPtr<ID3D12Fence> OpenFence(ID3D12Device& source, ID3D12Fence& fence, ID3D12Device& destination)
{
    HANDLE handle = nullptr;
    ThrowIfFailed(source.CreateSharedHandle(&fence, nullptr, GENERIC_ALL, nullptr, &handle));
    ComPtr<ID3D12Fence> opened;
    const auto hr = destination.OpenSharedHandle(handle, IID_PPV_ARGS(&opened));
    CloseHandle(handle);
    ThrowIfFailed(hr);
    return opened;
}
}

QueueSubmission::DeviceContext::DeviceContext(GDX12Device& device, std::unique_ptr<GDX12Device> owned)
    : Owned(std::move(owned)), Device(device), Identity(device.GetDevice())
{
    ThrowIfFailed(Identity.As(&ComIdentity));
    Queues[0].Queue = device.GetCommandQueue();
    for (auto queue : {Queue::Compute, Queue::Copy})
    {
        auto& timeline = Queues[Index(queue)];
        timeline.Owned = std::make_unique<GDX12CommandQueue>(&device, queue == Queue::Copy ?
            D3D12_COMMAND_LIST_TYPE_COPY : D3D12_COMMAND_LIST_TYPE_COMPUTE);
        timeline.Queue = timeline.Owned.get();
    }
}

QueueSubmission::QueueSubmission(GDX12Device& primary, std::vector<DeviceDesc> adapters, int primaryIndex)
    : _adapters(std::move(adapters)), _primaryIndex(primaryIndex), _devices(_adapters.size() + 1),
      _unavailable(_devices.size()), _incarnation(NextIncarnation.fetch_add(1))
{
    if (!Equal(primary.GetAdapterLuid(), _adapters.at(primaryIndex).AdapterLuid))
        throw std::invalid_argument("Bistro primary device differs from the selected DXGI adapter");
    for (size_t index = 0; index < _adapters.size(); ++index)
        _adapterLabels.push_back("[" + std::to_string(index) + "] " +
            std::filesystem::path(_adapters[index].Name).u8string());
    _devices[0] = std::make_unique<DeviceContext>(primary);
}

Id QueueSubmission::SelectAdapter(int hardwareIndex)
{
    if (hardwareIndex == -1 || hardwareIndex == _primaryIndex) return 0;
    if (hardwareIndex < 0 || size_t(hardwareIndex) >= _adapters.size())
        return 0;
    // Zero is the primary recording context. Other IDs map to absolute hardware indices plus one.
    const Id id = Id(hardwareIndex) + 1;
    std::lock_guard lock(_mutex);
    if (_devices[id]) return id;
    if (_unavailable[id]) return 0;

    try {
        const auto& adapter = _adapters[hardwareIndex];
        auto device = std::make_unique<GDX12Device>();
        ThrowIfFailed(device->Initialize(adapter.Adapter));
        if (!Equal(device->GetAdapterLuid(), adapter.AdapterLuid))
            throw std::runtime_error("Bistro remote device differs from the selected DXGI adapter");
        auto& remoteDevice = *device;
        auto context = std::make_unique<DeviceContext>(remoteDevice, std::move(device));
        auto& primary = *_devices[0];
        std::array<ComPtr<ID3D12Fence>, 3> primaryFences;
        for (size_t index = 0; index < primary.Queues.size(); ++index)
        {
            auto& local = primary.Queues[index];
            auto& remote = context->Queues[index];
            auto& fence = primaryFences[index];
            fence = local.Shared;
            if (!fence) fence = CreateSharedFence(*primary.Identity.Get());
            remote.PrimaryOnRemote = OpenFence(*primary.Identity.Get(), *fence.Get(), *context->Identity.Get());
            remote.Shared = CreateSharedFence(*context->Identity.Get());
            remote.OnPrimary = OpenFence(*context->Identity.Get(), *remote.Shared.Get(), *primary.Identity.Get());
        }
        for (size_t index = 0; index < primary.Queues.size(); ++index)
        {
            auto& local = primary.Queues[index];
            if (local.Shared) continue;
            // Cover primary tokens issued before a remote adapter was selected in F1.
            if (local.Issued)
                ThrowIfFailed(local.Queue->GetCommandQueue()->Signal(primaryFences[index].Get(), local.Issued));
            local.Shared = std::move(primaryFences[index]);
        }
        _devices[id] = std::move(context);
        return id;
    } catch (const DxException& error) {
        Log::Warning("RenderGraph", "Bistro adapter ", AdapterLabel(id), " unavailable: ", error.ToString());
    } catch (const std::exception& error) {
        Log::Warning("RenderGraph", "Bistro adapter ", AdapterLabel(id), " unavailable: ", error.what());
    }
    _unavailable[id] = true;
    return 0;
}

const std::string& QueueSubmission::AdapterLabel(Id id) const
{
    return _adapterLabels.at(id == 0 ? _primaryIndex : id - 1);
}

void QueueSubmission::BeginBenchmarkWaits()
{
    std::lock_guard<std::mutex> lock(_mutex);
    _benchmarkWaits.clear();
    _benchmarkPackets.clear();
    _benchmarking = true;
    _benchmarkEpoch.store(++_nextBenchmarkEpoch, std::memory_order_relaxed);
}

std::vector<QueueSubmission::BenchmarkWait> QueueSubmission::EndBenchmarkWaits()
{
    std::lock_guard<std::mutex> lock(_mutex);
    _benchmarking = false;
    _benchmarkEpoch.store(0, std::memory_order_relaxed);
    return std::exchange(_benchmarkWaits, {});
}

void QueueSubmission::AppendBenchmarkPackets(uint64_t epoch, std::vector<BenchmarkPacket> packets)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_benchmarking || epoch != _nextBenchmarkEpoch) return;
    _benchmarkPackets.insert(_benchmarkPackets.end(), std::make_move_iterator(packets.begin()),
        std::make_move_iterator(packets.end()));
}

std::vector<QueueSubmission::BenchmarkPacket> QueueSubmission::TakeBenchmarkPackets()
{
    std::lock_guard<std::mutex> lock(_mutex);
    return std::exchange(_benchmarkPackets, {});
}

std::vector<int> QueueSubmission::BenchmarkAdapterIndices() const
{
    std::vector<int> adapters;
    for (int i = 0; i < int(_adapters.size()); ++i)
        if (i != _primaryIndex) adapters.push_back(i);
    return adapters;
}

void QueueSubmission::LogFirstImport(const char* technique, CompletionToken source, CompletionToken imported) const
{
    assert(source && source.Device != 0 && imported && imported.Device == 0);
    Log::Info("RenderGraph", technique, " first import from ", AdapterLabel(source.Device), " on primary ",
              AdapterLabel(0), " exportFence=", source.Value, " importFence=", imported.Value);
}

QueueSubmission::~QueueSubmission() {
    // A failed Signal leaves unknown in-flight work. Quarantine every connected queue and fence.
    try { Check(); } catch(...) { _quarantined.store(true); }
    if(!_quarantined.load()) return;
    for (auto& [route, fence] : _peerFences) fence.Detach();
    for (auto& device : _devices) if (device)
    {
        device->Identity.Detach();
        for (auto& timeline : device->Queues)
        {
            timeline.Owned.release();
            timeline.Shared.Detach();
            timeline.OnPrimary.Detach();
            timeline.PrimaryOnRemote.Detach();
            if (timeline.Queue) timeline.Queue->GetCommandQueue()->AddRef();
        }
        device->Owned.release();
    }
}
GDX12Device& QueueSubmission::Device(Id id) const {
    const auto& context = _devices.at(id);
    if (!context) throw std::invalid_argument("unconfigured device context");
    return context->Device;
}
QueueSubmission::TransferBuffer QueueSubmission::CreateTransferBuffer(Id remote, uint64_t bytes) const {
    return CreateTransferBuffer(0, remote, bytes);
}
QueueSubmission::TransferBuffer QueueSubmission::CreateTransferBuffer(Id first, Id second, uint64_t bytes) const {
    if (first == second || !bytes) throw std::invalid_argument("invalid cross-adapter buffer");
    auto* primary = Device(first).GetDevice().Get();
    auto* secondary = Device(second).GetDevice().Get();
    const auto desc = CD3DX12_RESOURCE_DESC::Buffer(bytes, D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER);
    const auto a = primary->GetResourceAllocationInfo(0, 1, &desc);
    const auto b = secondary->GetResourceAllocationInfo(0, 1, &desc);
    if (a.SizeInBytes == UINT64_MAX || b.SizeInBytes == UINT64_MAX)
        throw std::runtime_error("cross-adapter buffer allocation is unsupported");
    D3D12_HEAP_DESC heap{};
    heap.SizeInBytes = (std::max)(a.SizeInBytes, b.SizeInBytes);
    heap.Alignment = (std::max)(a.Alignment, b.Alignment);
    heap.Properties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    heap.Flags = D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER;
    TransferBuffer buffer;
    ThrowIfFailed(secondary->CreateHeap(&heap, IID_PPV_ARGS(&buffer.Heaps[1])));
    HANDLE handle = nullptr;
    ThrowIfFailed(secondary->CreateSharedHandle(buffer.Heaps[1].Get(), nullptr, GENERIC_ALL, nullptr, &handle));
    const auto opened = primary->OpenSharedHandle(handle, IID_PPV_ARGS(&buffer.Heaps[0]));
    CloseHandle(handle);
    ThrowIfFailed(opened);
    for (Id id = 0; id < 2; ++id) {
        const auto device = id == 0 ? first : second;
        ThrowIfFailed(Device(device).GetDevice()->CreatePlacedResource(buffer.Heaps[id].Get(), 0, &desc,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&buffer.Resources[id])));
        ValidateResource(device, buffer.Resources[id].Get());
    }
    return buffer;
}
void QueueSubmission::Check() const {
    if(_quarantined.load()) throw std::runtime_error("submission domain quarantined");
    try {
        for (const auto& device : _devices) if (device)
        {
            if (device->Device.GetDevice().Get() != device->Identity.Get())
                throw std::runtime_error("device context incarnation changed");
            ThrowIfFailed(device->Identity->GetDeviceRemovedReason());
            for (const auto& timeline : device->Queues)
                if (timeline.Poisoned) throw std::runtime_error("submission domain quarantined after native failure");
        }
    } catch(...) { _quarantined.store(true); throw; }
}
void QueueSubmission::CheckHealth() const { std::lock_guard<std::mutex> lock(_mutex); Check(); }
const QueueSubmission::Timeline& QueueSubmission::Resolve(CompletionToken token) const {
    Device(token.Device);
    const auto& t=_devices[token.Device]->Queues[Index(token.Target)];
    if(!token || token.Incarnation!=_incarnation || token.Value>t.Issued)
        throw std::invalid_argument("stale or unissued completion token");
    return t;
}
ID3D12Fence* QueueSubmission::CompletionFence(CompletionToken token) const {
    const auto& timeline=Resolve(token);
    return (timeline.Shared ? timeline.Shared : timeline.Queue->GetFence()).Get();
}
void QueueSubmission::NotifyOnCompletion(CompletionToken token, HANDLE event) const {
    if(!event) throw std::invalid_argument("missing completion event");
    std::lock_guard<std::mutex> lock(_mutex); Check();
    ThrowIfFailed(CompletionFence(token)->SetEventOnCompletion(token.Value, event));
}
bool QueueSubmission::Complete(CompletionToken token) const {
    std::lock_guard<std::mutex> lock(_mutex); Check();
    const auto value=CompletionFence(token)->GetCompletedValue();
    if(value==UINT64_MAX) { _quarantined.store(true); ThrowIfFailed(DXGI_ERROR_DEVICE_REMOVED); }
    return value>=token.Value;
}
void QueueSubmission::Wait(CompletionToken token) const {
    // Polling also observes removal of the OTHER device behind a shared wait.
    while(!Complete(token)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
}
GraphResourceInfo QueueSubmission::ValidateResource(Id id, ID3D12Resource* resource) const {
    const auto info = GraphResourceInfo::Read(resource);
    const auto& device = Device(id);
    if (info.Owner != _devices[id]->ComIdentity.Get() || !Equal(info.Adapter, device.GetAdapterLuid()))
        throw std::invalid_argument("resource bound to wrong physical device/incarnation");
    return info;
}
CompletionToken QueueSubmission::Submit(Id device,BistroRender::Queue target,
    const std::vector<ID3D12CommandList*>* packets,size_t count,const std::vector<CompletionToken>& waits,GDX12CommandList* external) {
    std::lock_guard<std::mutex> lock(_mutex); Check();
    Device(device); auto& t=_devices[device]->Queues[Index(target)];
    if (!count && !external)
        throw std::invalid_argument("empty/oversized packet submission");
    for (size_t i = 0; i < count; ++i) {
        const auto& lists = packets[i];
        if (lists.empty() || lists.size() > UINT_MAX) throw std::invalid_argument("invalid packet size");
        for(auto* list:lists) {
            ComPtr<ID3D12Device> owner; ThrowIfFailed(list->GetDevice(IID_PPV_ARGS(&owner)));
            ComPtr<IUnknown> a,b;
            ThrowIfFailed(owner.As(&a)); ThrowIfFailed(Device(device).GetDevice().As(&b));
            if(a.Get()!=b.Get()) throw std::invalid_argument("foreign command list device");
        }
    }
    for (auto token : waits) {
        const auto& producer = Resolve(token);
        if (device && token.Device && token.Device != device)
        {
            auto& fence = _peerFences[{token.Device, device, Index(token.Target)}];
            if (!fence) fence = OpenFence(*_devices[token.Device]->Identity.Get(), *producer.Shared.Get(), *_devices[device]->Identity.Get());
        }
    }
    try {
        t.Poisoned=true;
        for(auto token:waits) {
            const auto& producer=Resolve(token);
            if(token.Device==device && token.Target==target) continue;
            auto* fence = token.Device == device ? producer.Queue->GetFence().Get() :
                device == 0 ? producer.OnPrimary.Get() :
                token.Device == 0 ? _devices[device]->Queues[Index(token.Target)].PrimaryOnRemote.Get() :
                _peerFences.at({token.Device, device, Index(token.Target)}).Get();
            if(!fence) throw std::invalid_argument("missing cross-device fence handle");
            ThrowIfFailed(t.Queue->GetCommandQueue()->Wait(fence,token.Value));
            if (_benchmarking && token.Device != device)
            {
                const auto wait = std::find_if(_benchmarkWaits.begin(), _benchmarkWaits.end(), [&](const auto& w)
                {
                    return w.From == token.Device && w.To == device && w.SourceQueue == token.Target && w.TargetQueue == target;
                });
                if (wait == _benchmarkWaits.end())
                    _benchmarkWaits.push_back({token.Device, device, token.Target, target, token.Value, 1});
                else ++wait->Count;
            }
        }
        uint64_t value=0;
        if(external) { t.Queue->ExecuteCommandList(external); value=external->FenceValue; }
        else {
            // The executor has already grouped lists that can share a native execution boundary.
            for (size_t i = 0; i < count; ++i) {
                const auto& lists = packets[i];
                t.Queue->GetCommandQueue()->ExecuteCommandLists(UINT(lists.size()),lists.data());
            }
            value=t.Queue->Signal();
        }
        if(t.Shared) ThrowIfFailed(t.Queue->GetCommandQueue()->Signal(t.Shared.Get(),value));
        t.Issued=value; t.Poisoned=false;
        return {device,target,value,_incarnation};
    } catch(...) { _quarantined.store(true); throw; } // No token is published.
}
}

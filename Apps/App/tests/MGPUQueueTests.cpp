#include "App.Base/App.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12CommandQueue.h"
#include <d3d12sdklayers.h>
#include <dxc/dxcapi.h>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>

using namespace GDX12Submission;
static void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class F> static void Reject(F action, const char* message)
{
    bool rejected = false;
    try { action(); } catch (const std::invalid_argument&) { rejected = true; }
    catch (const std::logic_error&) { rejected = true; }
    catch (const std::overflow_error&) { rejected = true; }
    Require(rejected, message); // A native HRESULT failure is NOT successful input validation.
}
class TestApp final : public App { public: TestApp() : App(GetModuleHandleW(nullptr)) {} };
static constexpr uint64_t Max = (std::numeric_limits<uint64_t>::max)();

static void TestModel()
{
    constexpr CompletionPoint invalid;
    constexpr TimelineId timeline{{{AdapterId{1}, QueueKind::Copy}, 1}, 2};
    static_assert(!invalid.IsValid() && !invalid.IsReachedBy({timeline, Max}), "Invalid token completed");
    static_assert(!(timeline == TimelineId{{{AdapterId{1}, QueueKind::Compute}, 1}, 2}), "Queue kind lost");
    static_assert(!(timeline == TimelineId{{{AdapterId{2}, QueueKind::Copy}, 1}, 2}), "Adapter lost");
    static_assert(!(timeline == TimelineId{{{AdapterId{1}, QueueKind::Copy}, 2}, 2}), "Queue incarnation lost");
    static_assert(!(timeline == TimelineId{{{AdapterId{1}, QueueKind::Copy}, 1}, 3}), "Fence incarnation lost");
    GDX12Device device;
    Require(device.GetSubmissionAdapterId() == AdapterId::Invalid, "Fresh device has an identity");
    Reject([&] { GDX12CommandQueue queue(nullptr); }, "Null device accepted");
    Reject([&] { GDX12CommandList list(nullptr, D3D12_COMMAND_LIST_TYPE_COPY); }, "Null command list device accepted");
    Reject([&] { GDX12CommandList list(&device, D3D12_COMMAND_LIST_TYPE_COPY); }, "Uninitialized command list device accepted");
    Reject([&] { GDX12CommandQueue queue(&device); }, "Uninitialized device accepted");
    Reject([&] { GDX12CommandQueue queue(nullptr, D3D12_COMMAND_LIST_TYPE_BUNDLE); }, "BUNDLE queue accepted");
    Reject([&] { device.TryGetCopyQueue(); }, "COPY requested before device initialization");
    Reject([&] { device.TryGetComputeQueue(); }, "COMPUTE requested before device initialization");
    Require(device.Initialize(nullptr) == E_INVALIDARG, "Null adapter reached native device creation");
    device.Reset(); device.Reset();
    std::cout << "CPU/model invalid inputs passed without native waits.\n";
}

static ComPtr<ID3D12Resource> Buffer(GDX12Device& device, D3D12_HEAP_TYPE type, UINT64 bytes)
{
    CD3DX12_HEAP_PROPERTIES heap(type);
    auto desc = CD3DX12_RESOURCE_DESC::Buffer(bytes);
    ComPtr<ID3D12Resource> buffer;
    ThrowIfFailed(device.GetDevice()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ :
        type == D3D12_HEAP_TYPE_READBACK ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_COMMON,
        nullptr, IID_PPV_ARGS(&buffer)));
    return buffer;
}

// This is a fixture gate, not a submission timeline. Its CPU release must never issue a CompletionPoint.
class Gate
{
public:
    Gate(GDX12Device& device, GDX12CommandQueue& queue)
    {
        ThrowIfFailed(device.GetDevice()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&Fence)));
        ThrowIfFailed(queue.GetCommandQueue()->Wait(Fence.Get(), 1));
        try { Watchdog = std::thread([this]
        {
            std::unique_lock<std::mutex> lock(Mutex);
            if (!Changed.wait_for(lock, std::chrono::seconds(5), [this] { return Released; }))
            {
                TimedOut = true;
                ReleaseResult = Fence->Signal(1);
            }
        }); }
        catch (...)
        {
            const HRESULT result = Fence->Signal(1);
            if (FAILED(result)) OutputDebugStringA("Fixture gate release failed after watchdog creation failure.\n");
            throw;
        }
    }
    void Open()
    {
        { std::lock_guard<std::mutex> lock(Mutex); Released = true; }
        Changed.notify_one();
        Watchdog.join();
        ThrowIfFailed(ReleaseResult);
        ThrowIfFailed(Fence->Signal(1));
        Require(!TimedOut, "Queue Wait or allocator reuse blocked the CPU until the watchdog released the producer");
    }
    ~Gate()
    {
        if (Watchdog.joinable())
        {
            { std::lock_guard<std::mutex> lock(Mutex); Released = true; }
            Changed.notify_one(); Watchdog.join();
            const HRESULT result = Fence->Signal(1);
            if (FAILED(result)) OutputDebugStringA("Fixture gate release failed during exception cleanup.\n");
        }
    }
private:
    ComPtr<ID3D12Fence> Fence;
    std::mutex Mutex;
    std::condition_variable Changed;
    bool Released = false, TimedOut = false;
    HRESULT ReleaseResult = S_OK;
    std::thread Watchdog;
};

static void TestLocal(GDX12Device& device)
{
    auto& direct = *device.GetCommandQueue();
    auto* computePtr = device.TryGetComputeQueue();
    auto* copyPtr = device.TryGetCopyQueue();
    Require(computePtr && copyPtr, "Local COMPUTE/COPY capability required by P1 fixture");
    auto& compute = *computePtr; auto& copy = *copyPtr;
    Require(direct.GetIdentity().Location.Adapter == device.GetSubmissionAdapterId(), "Wrong adapter identity");
    Require(compute.GetIdentity().Location.Queue == QueueKind::Compute && copy.GetIdentity().Location.Queue == QueueKind::Copy,
        "Wrong queue kinds");
    Require(!direct.GetLastCompletionPoint().IsValid(), "Queue issued a token before Signal");
    auto pD = direct.SignalCompletion(), pC = compute.SignalCompletion(), pCopy = copy.SignalCompletion();
    Require(pD.Value() == 1 && pC.Value() == 1 && pCopy.Value() == 1, "Unexpected initial signal values");
    Require(!pD.IsReachedBy({pC.Timeline(), 100}) && !pCopy.IsReachedBy({pD.Timeline(), 100}),
        "Numerically larger foreign fence retired a token");
    Require(!pD.IsReachedBy({pD.Timeline(), Max}), "Device removal retired a token");
    Require(!pD.IsReachedBy({pD.Timeline(), 0}), "Pending point retired too early");
    Reject([&] { direct.WaitFor({}); }, "Invalid completion token accepted");
    Reject([&] { direct.GetCompletionProgress({}); }, "Invalid timeline accepted");
    auto unknown = pD.Timeline(); unknown.Instance += 1000000;
    Reject([&] { direct.GetCompletionProgress(unknown); }, "Unknown timeline accepted");
    Reject([&] { direct.WaitForQueue(compute, compute.FenceValue + 1); }, "Unissued future fence accepted");
    Reject([&] { direct.WaitForQueue(compute, 0); }, "Zero fence accepted");
    Reject([&] { direct.CPUWaitForFenceValue(direct.FenceValue + 1); }, "CPU waited for an unissued value");
    Reject([&] { direct.WaitForOtherFence(1); }, "Unverified legacy imported fence accepted");
    Reject([&] { direct.ImportTimeline(direct); }, "Self import accepted");
    const auto savedCounter = direct.FenceValue;
    direct.FenceValue = Max - 1;
    Reject([&] { direct.SignalCompletion(); }, "Fence sentinel overflow accepted");
    direct.FenceValue = Max;
    Reject([&] { direct.SignalCompletion(); }, "Fence counter wrapped");
    direct.FenceValue = savedCounter;
    Require(direct.GetLastCompletionPoint().Value() == pD.Value(), "Rejected Signal changed completion token");

    auto* copyList = copy.GetCommandList();
    Reject([&] { copyList->Dispatch(1, 1, 1); }, "COPY Dispatch accepted");
    Reject([&] { copyList->SetPipelineState1({}); }, "COPY DXR pipeline accepted");
    Reject([&] { copyList->DispatchRays(nullptr); }, "COPY DXR dispatch accepted");
    Reject([&] { copyList->BuildRaytracingAccelerationStructure(nullptr); }, "COPY DXR build accepted");
    Reject([&] { copyList->DrawInstanced(3, 1, 0, 0); }, "COPY draw accepted");
    Reject([&] { copyList->SetDescriptorHeaps({}); }, "COPY descriptor heaps accepted");
    Reject([&] { copyList->ResourceBarrier({CD3DX12_RESOURCE_BARRIER::UAV(nullptr)}); }, "COPY UAV barrier accepted");
    auto dummy = Buffer(device, D3D12_HEAP_TYPE_DEFAULT, 256);
    Reject([&] { copyList->ResourceBarrier({CD3DX12_RESOURCE_BARRIER::Transition(dummy.Get(),
        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)}); }, "COPY shader state accepted");
    auto* computeList = compute.GetCommandList();
    Reject([&] { computeList->DrawInstanced(3, 1, 0, 0); }, "COMPUTE draw accepted");
    Reject([&] { direct.ExecuteCommandList(copyList); }, "Mismatched list type accepted");
    GDX12CommandQueue otherCopy(&device, D3D12_COMMAND_LIST_TYPE_COPY);
    Reject([&] { otherCopy.ExecuteCommandList(copyList); }, "Foreign allocator ownership accepted");
    std::vector<GDX12CommandList*> empty, duplicate{copyList, copyList}, nullList{nullptr};
    Reject([&] { copy.ExecuteCommandLists(empty); }, "Empty batch accepted");
    Reject([&] { copy.ExecuteCommandLists(duplicate); }, "Duplicate batch accepted");
    Reject([&] { copy.ExecuteCommandLists(nullList); }, "Null batch list accepted");

    const UINT64 bytes = 256;
    auto upload = Buffer(device, D3D12_HEAP_TYPE_UPLOAD, bytes);
    auto a = Buffer(device, D3D12_HEAP_TYPE_DEFAULT, bytes);
    auto b = Buffer(device, D3D12_HEAP_TYPE_DEFAULT, bytes);
    auto c = Buffer(device, D3D12_HEAP_TYPE_DEFAULT, bytes);
    auto readback = Buffer(device, D3D12_HEAP_TYPE_READBACK, bytes);
    std::array<uint32_t, bytes / sizeof(uint32_t)> expected{};
    for (size_t i = 0; i < expected.size(); ++i) expected[i] = 0x13570000u + static_cast<uint32_t>(i);
    void* mapped = nullptr; D3D12_RANGE noRead{0, 0};
    ThrowIfFailed(upload->Map(0, &noRead, &mapped));
    std::memcpy(mapped, expected.data(), bytes); upload->Unmap(0, nullptr);
    auto* directList = direct.GetCommandList();
    // Buffers promote from COMMON and decay at ExecuteCommandLists boundaries, including COPY handoff.
    directList->CopyResource(a.Get(), upload.Get()); direct.ExecuteCommandList(directList);
    const auto produced = direct.GetLastCompletionPoint();
    compute.WaitFor(produced); computeList->CopyResource(b.Get(), a.Get()); compute.ExecuteCommandList(computeList);
    const auto computed = compute.GetLastCompletionPoint();
    copy.WaitFor(computed); copyList->CopyResource(c.Get(), b.Get()); copy.ExecuteCommandList(copyList);
    const auto copied = copy.GetLastCompletionPoint();
    direct.WaitFor(copied); directList = direct.GetCommandList();
    directList->CopyResource(readback.Get(), c.Get()); direct.ExecuteCommandList(directList);
    const auto consumed = direct.GetLastCompletionPoint(); direct.CPUWaitForFenceValue(consumed.Value());
    D3D12_RANGE read{0, bytes}; ThrowIfFailed(readback->Map(0, &read, &mapped));
    const bool equal = std::memcmp(mapped, expected.data(), bytes) == 0;
    readback->Unmap(0, &noRead); Require(equal, "DIRECT -> COMPUTE -> COPY -> DIRECT data corruption");
    Require(consumed.IsReachedBy(direct.GetCompletionProgress()), "Final DIRECT did not complete");
    Require(computed.IsReachedBy(copy.GetCompletionProgress(computed.Timeline())), "Local producer progress lost provenance");

    // Keep producer incomplete so token completion cannot conceal CPU blocking or premature allocator reuse.
    {
        Gate gate(device, compute);
        auto* busy = compute.GetCommandList(); compute.ExecuteCommandList(busy);
        const auto pending = compute.GetLastCompletionPoint();
        direct.WaitFor(pending);
        Require(!pending.IsReachedBy(compute.GetCompletionProgress()), "Gate did not delay producer");
        Reject([&] { busy->Reset(); }, "Pending allocator reset accepted");
        Reject([&] { compute.ExecuteCommandList(busy); }, "Submitted list re-executed");
        const auto publicValue = busy->FenceValue;
        busy->FenceValue = 0; // Legacy observations must not control native allocator retirement.
        auto* available = compute.GetCommandList();
        busy->FenceValue = publicValue;
        Require(available != busy && available->GetCommandAllocator().Get() != busy->GetCommandAllocator().Get(),
            "Pending allocator recycled");
        compute.ExecuteCommandList(available);
        gate.Open();
        compute.Flush(); direct.Flush();
        auto* recycled = compute.GetCommandList();
        Require(recycled == busy || recycled == available, "Completed allocator was not reused");
        compute.ExecuteCommandList(recycled); compute.Flush();
    }
    otherCopy.Reset();
    Reject([&] { otherCopy.SignalCompletion(); }, "Reset queue issued a token");
    Reject([&] { otherCopy.GetCommandList(); }, "Reset queue issued a command list");
    Reject([&] { direct.ImportTimeline(otherCopy); }, "Reset producer imported");
    std::cout << "Single physical GPU DIRECT/COMPUTE/COPY chain, provenance, negative commands and retirement passed.\n";
}

static void TestComputeDxrBinding(GDX12Device& device)
{
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options{};
    ThrowIfFailed(device.GetDevice()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options, sizeof(options)));
    if (options.RaytracingTier == D3D12_RAYTRACING_TIER_NOT_SUPPORTED)
    {
        std::cout << "SKIP positive COMPUTE DXR binding: adapter has no DXR capability.\n";
        return;
    }
    const char shader[] = "[shader(\"raygeneration\")] void RayGen() {}";
    ComPtr<IDxcCompiler3> compiler;
    ThrowIfFailed(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)));
    const DxcBuffer input{shader, sizeof(shader) - 1, DXC_CP_UTF8};
    const wchar_t* arguments[]{ L"-T", L"lib_6_3" };
    ComPtr<IDxcResult> result;
    ThrowIfFailed(compiler->Compile(&input, arguments, 2, nullptr, IID_PPV_ARGS(&result)));
    HRESULT status = E_FAIL; ThrowIfFailed(result->GetStatus(&status)); ThrowIfFailed(status);
    ComPtr<IDxcBlob> library; ThrowIfFailed(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&library), nullptr));
    CD3DX12_ROOT_SIGNATURE_DESC rootDesc; rootDesc.Init(0, nullptr);
    ComPtr<ID3DBlob> serialized, errors;
    ThrowIfFailed(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors));
    ComPtr<ID3D12RootSignature> root;
    ThrowIfFailed(device.GetDevice()->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&root)));
    D3D12_GLOBAL_ROOT_SIGNATURE global{root.Get()};
    D3D12_DXIL_LIBRARY_DESC dxil{}; dxil.DXILLibrary = {library->GetBufferPointer(), library->GetBufferSize()};
    D3D12_RAYTRACING_SHADER_CONFIG shaderConfig{4, 8};
    D3D12_RAYTRACING_PIPELINE_CONFIG pipelineConfig{1};
    D3D12_STATE_SUBOBJECT subobjects[]{
        {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &dxil},
        {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &global},
        {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shaderConfig},
        {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipelineConfig}
    };
    D3D12_STATE_OBJECT_DESC desc{D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, 4, subobjects};
    ComPtr<ID3D12StateObject> state;
    ThrowIfFailed(device.GetDevice()->CreateStateObject(&desc, IID_PPV_ARGS(&state)));
    auto* queue = device.TryGetComputeQueue(); Require(queue, "COMPUTE queue missing for DXR fixture");
    auto* commands = queue->GetCommandList();
    commands->SetPipelineState1(state); queue->ExecuteCommandList(commands); queue->Flush();
    std::cout << "Positive COMPUTE SetPipelineState1 passed.\n";
}

static bool SameLuid(LUID a, LUID b) { return a.LowPart == b.LowPart && a.HighPart == b.HighPart; }
static bool Unsupported(HRESULT hr) { return hr == DXGI_ERROR_UNSUPPORTED || hr == E_NOTIMPL; }
struct ScopedHandle { HANDLE Value = nullptr; ~ScopedHandle() { if (Value) CloseHandle(Value); } };

static void TestCrossAdapter(GDX12Device& primary, GDX12Device& secondary)
{
    Require(!SameLuid(primary.GetAdapterLuid(), secondary.GetAdapterLuid()), "Two queues on one physical GPU are not MGPU");
    std::cout << "Physical adapter LUIDs: " << primary.GetAdapterLuid().HighPart << ':' << primary.GetAdapterLuid().LowPart
        << " -> " << secondary.GetAdapterLuid().HighPart << ':' << secondary.GetAdapterLuid().LowPart << '\n';
    GDX12CommandQueue producer(&primary, D3D12_COMMAND_LIST_TYPE_COPY, true);
    GDX12CommandQueue consumer(&secondary, D3D12_COMMAND_LIST_TYPE_COPY, true);
    auto beforeImport = producer.SignalCompletion();
    Reject([&] { consumer.WaitFor(beforeImport); }, "Unimported remote token reached native Wait");
    Reject([&] { consumer.ImportTimeline(*primary.GetCommandQueue()); }, "Nonshared remote timeline exported");
    const auto exported = consumer.ImportTimeline(producer);
    Require(exported == producer.GetTimelineIdentity(), "Import changed producer identity");
    Require(consumer.ImportTimeline(producer) == exported, "Repeated import changed identity");
    producer.ImportTimeline(consumer);
    {
        GDX12CommandQueue wrongProducer(&primary, D3D12_COMMAND_LIST_TYPE_COPY, true);
        const auto wrongPoint = wrongProducer.SignalCompletion();
        Require(wrongPoint.Value() == beforeImport.Value(), "Foreign timeline numeric equality fixture failed");
        Reject([&] { consumer.WaitFor(wrongPoint); }, "Import of one producer authorized another producer with equal value");
        const auto savedView = consumer.GetOtherFence();
        consumer.GetOtherFence() = consumer.GetFence();
        Reject([&] { consumer.WaitForOtherFence(beforeImport.Value()); }, "Mismatched legacy view accepted");
        consumer.GetOtherFence() = savedView;
    }
    consumer.WaitFor(beforeImport);

    constexpr UINT64 bytes = 256;
    auto sharedDesc = CD3DX12_RESOURCE_DESC::Buffer(bytes, D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER);
    const auto primaryInfo = primary.GetDevice()->GetResourceAllocationInfo(0, 1, &sharedDesc);
    const auto secondaryInfo = secondary.GetDevice()->GetResourceAllocationInfo(0, 1, &sharedDesc);
    Require(primaryInfo.SizeInBytes != Max && secondaryInfo.SizeInBytes != Max, "Invalid cross-adapter allocation footprint");
    D3D12_HEAP_DESC heapDesc{};
    heapDesc.Alignment = (std::max)(primaryInfo.Alignment, secondaryInfo.Alignment);
    heapDesc.SizeInBytes = (std::max)(primaryInfo.SizeInBytes, secondaryInfo.SizeInBytes);
    heapDesc.Properties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    heapDesc.Flags = D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER;
    ComPtr<ID3D12Heap> sourceHeap, importedHeap;
    ThrowIfFailed(primary.GetDevice()->CreateHeap(&heapDesc, IID_PPV_ARGS(&sourceHeap)));
    {
        ScopedHandle handle;
        ThrowIfFailed(primary.GetDevice()->CreateSharedHandle(sourceHeap.Get(), nullptr, GENERIC_ALL, nullptr, &handle.Value));
        ThrowIfFailed(secondary.GetDevice()->OpenSharedHandle(handle.Value, IID_PPV_ARGS(&importedHeap)));
    }
    ComPtr<ID3D12Resource> sourceShared, importedShared;
    ThrowIfFailed(primary.GetDevice()->CreatePlacedResource(sourceHeap.Get(), 0, &sharedDesc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&sourceShared)));
    ThrowIfFailed(secondary.GetDevice()->CreatePlacedResource(importedHeap.Get(), 0, &sharedDesc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&importedShared)));
    auto upload = Buffer(primary, D3D12_HEAP_TYPE_UPLOAD, bytes);
    auto source = Buffer(primary, D3D12_HEAP_TYPE_DEFAULT, bytes);
    auto local = Buffer(secondary, D3D12_HEAP_TYPE_DEFAULT, bytes);
    auto readback = Buffer(secondary, D3D12_HEAP_TYPE_READBACK, bytes);
    std::array<uint32_t, bytes / sizeof(uint32_t)> payload{};
    D3D12_RANGE noRead{0, 0}, read{0, bytes};
    // Different sequence/checksum payloads exercise repeated slot release, not just fence reachability.
    for (uint32_t generation = 1; generation <= 24; ++generation)
    {
        for (size_t i = 0; i < payload.size(); ++i) payload[i] = generation * 7919u + static_cast<uint32_t>(i * 31u);
        void* mapped = nullptr; ThrowIfFailed(upload->Map(0, &noRead, &mapped));
        std::memcpy(mapped, payload.data(), bytes); upload->Unmap(0, nullptr);
        auto* write = producer.GetCommandList();
        write->CopyResource(source.Get(), upload.Get());
        // Two copies in one list require an explicit COPY_DEST -> COPY_SOURCE transition.
        write->ResourceBarrier({CD3DX12_RESOURCE_BARRIER::Transition(source.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE)});
        write->CopyResource(sourceShared.Get(), source.Get()); producer.ExecuteCommandList(write);
        const auto ready = producer.GetLastCompletionPoint(); consumer.WaitFor(ready);
        auto* copy = consumer.GetCommandList(); copy->CopyResource(local.Get(), importedShared.Get());
        consumer.ExecuteCommandList(copy); const auto released = consumer.GetLastCompletionPoint();
        producer.WaitFor(released);
        auto* reader = consumer.GetCommandList(); reader->CopyResource(readback.Get(), local.Get());
        consumer.ExecuteCommandList(reader); consumer.CPUWaitForFenceValue(consumer.GetLastCompletionPoint().Value());
        ThrowIfFailed(readback->Map(0, &read, &mapped));
        const bool equal = std::memcmp(mapped, payload.data(), bytes) == 0;
        readback->Unmap(0, &noRead); Require(equal, "Cross-adapter sequence/checksum transfer corrupted");
        Require(ready.IsReachedBy(consumer.GetCompletionProgress(ready.Timeline())), "Imported progress has wrong provenance");
    }
    producer.Flush(); consumer.Flush();
    const auto old = producer.GetLastCompletionPoint(); producer.Reset();
    Reject([&] { consumer.WaitFor(old); }, "Reset producer token accepted through imported view");
    Reject([&] { consumer.GetCompletionProgress(old.Timeline()); }, "Stale imported progress accepted");

    // Warm up native queue/import teardown, then require bounded handle ownership over many fresh timelines.
    auto importOnce = [&]
    {
        GDX12CommandQueue fresh(&primary, D3D12_COMMAND_LIST_TYPE_COPY, true);
        consumer.ImportTimeline(fresh); const auto point = fresh.SignalCompletion();
        consumer.WaitFor(point); consumer.Flush(); fresh.Reset();
    };
    importOnce();
    DWORD handlesBefore = 0, handlesAfter = 0;
    Require(GetProcessHandleCount(GetCurrentProcess(), &handlesBefore), "Cannot read process handle count");
    for (int i = 0; i < 32; ++i) importOnce();
    Require(GetProcessHandleCount(GetCurrentProcess(), &handlesAfter), "Cannot read process handle count");
    Require(handlesAfter <= handlesBefore + 4, "Cross-adapter imports leaked handles");
    consumer.Reset();
    std::cout << "Two physical GPUs: cross-adapter Signal/Wait, 24 payload generations, stale import and bounded handles passed.\n";
}

static void CheckDebug(ID3D12InfoQueue& info)
{
    for (UINT64 i = 0; i < info.GetNumStoredMessages(); ++i)
    {
        SIZE_T size = 0; ThrowIfFailed(info.GetMessage(i, nullptr, &size));
        std::vector<BYTE> storage(size);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        ThrowIfFailed(info.GetMessage(i, message, &size));
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) throw std::runtime_error(message->pDescription);
    }
}

static bool TestSecondPhysicalAdapter(GDX12Device& primary)
{
    // Enumerate by physical LUID here: the legacy descriptor helper deduplicates by DeviceId.
    auto& factory = GDX12DeviceFactory::GetFactory();
    for (UINT index = 0;; ++index)
    {
        ComPtr<IDXGIAdapter4> adapter;
        const auto hr = factory->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        ThrowIfFailed(hr);
        DXGI_ADAPTER_DESC1 desc{}; ThrowIfFailed(adapter->GetDesc1(&desc));
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || SameLuid(desc.AdapterLuid, primary.GetAdapterLuid())) continue;
        const auto supported = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device14), nullptr);
        if (FAILED(supported))
        {
            if (!Unsupported(supported)) ThrowIfFailed(supported);
            std::wcout << L"Second adapter unavailable for D3D12: " << desc.Description << L" HRESULT=" << supported << '\n';
            continue;
        }
        GDX12Device secondary; secondary.Role = DEVICE_ROLE_SECONDARY;
        ThrowIfFailed(secondary.Initialize(adapter));
        ComPtr<ID3D12InfoQueue> info; ThrowIfFailed(secondary.GetDevice().As(&info)); info->ClearStoredMessages();
        try { TestCrossAdapter(primary, secondary); }
        catch (const DxException& error)
        {
            if (!Unsupported(error.ErrorCode)) throw;
            std::wcout << L"Second adapter lacks required cross-adapter capability: " << desc.Description << L" " << error.ToString() << '\n';
            secondary.Reset(); continue;
        }
        CheckDebug(*info.Get()); secondary.Reset(); CheckDebug(*info.Get()); return true;
    }
    std::cout << "SKIP physical two-GPU fixture: no second compatible physical adapter; local queues do not count as MGPU.\n";
    return false;
}

int main(int argc, char** argv)
{
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    try
    {
        TestModel();
        bool modelOnly = false, gbv = false, requireTwo = false;
        for (int i = 1; i < argc; ++i)
        {
            const std::string_view option(argv[i]);
            if (option == "--model-only") modelOnly = true;
            else if (option == "--gpu-validation") gbv = true;
            else if (option == "--require-two-adapters") requireTwo = true;
            else throw std::invalid_argument("Unknown test argument");
        }
        if (modelOnly) return 0;
        TestApp app;
        ComPtr<ID3D12Debug> debug; ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))); debug->EnableDebugLayer();
        if (gbv)
        {
            ComPtr<ID3D12Debug1> validation; ThrowIfFailed(debug.As(&validation));
            validation->SetEnableGPUBasedValidation(TRUE);
        }
        auto adapter = GDX12DeviceFactory::GetMostPerformantAdapter();
        GDX12Device device; ThrowIfFailed(device.Initialize(adapter));
        ComPtr<ID3D12InfoQueue> info; ThrowIfFailed(device.GetDevice().As(&info)); info->ClearStoredMessages();
        TestLocal(device);
        TestComputeDxrBinding(device);
        const bool two = TestSecondPhysicalAdapter(device);
        device.GetCommandQueue()->Flush(); CheckDebug(*info.Get());
        const auto oldAdapter = device.GetSubmissionAdapterId();
        const auto oldQueue = device.GetCommandQueue()->GetIdentity();
        const auto oldPoint = device.GetCommandQueue()->GetLastCompletionPoint();
        // Reset must also invalidate caller-owned queues before native device release.
        GDX12CommandQueue external(&device, D3D12_COMMAND_LIST_TYPE_COPY);
        external.ExecuteCommandList(external.GetCommandList());
        device.Reset(); Require(!external.GetIdentity().IsValid(), "Device reset left external queue active");
        CheckDebug(*info.Get()); info.Reset();
        ThrowIfFailed(device.Initialize(adapter));
        Require(!(device.GetSubmissionAdapterId() == oldAdapter) && !(device.GetCommandQueue()->GetIdentity() == oldQueue),
            "Recreated device/queue reused identities");
        Reject([&] { device.GetCommandQueue()->WaitFor(oldPoint); }, "Previous device incarnation token accepted");
        Require(!device.GetCommandQueue()->GetLastCompletionPoint().IsValid(), "Recreated queue inherited a completion token");
        ThrowIfFailed(device.GetDevice().As(&info)); device.Reset(); CheckDebug(*info.Get());
        if (requireTwo) Require(two, "Two-adapter acceptance requested but second compatible physical adapter unavailable");
        std::cout << "P1 queue tests passed; clean D3D12 debug layer. Physical two-GPU=" << (two ? "PASS" : "SKIP") << '\n';
        return 0;
    }
    catch (const DxException& error) { std::wcerr << error.ToString() << '\n'; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
    return 1;
}

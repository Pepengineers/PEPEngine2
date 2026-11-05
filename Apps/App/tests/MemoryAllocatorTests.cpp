#include "App.Base/App.h"
#include "Engine.RendererDX12/GDX12DeviceFactory.h"
#include "Engine.RendererDX12/GDX12DeviceResources.h"
#include "Engine.RendererDX12/GDX12CommandQueue.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include "Engine.RendererDX12/GDX12TextureResource.h"
#include <d3d12sdklayers.h>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <future>
#include <thread>

static void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

class TestApp final : public App
{
public:
    TestApp() : App(GetModuleHandleW(nullptr)) {}
};

static D3D12MA::Statistics Statistics(GDX12Device& device)
{
    D3D12MA::TotalStatistics stats = {};
    device.GetMemoryAllocator()->CalculateStatistics(&stats);
    return stats.Total.Stats;
}

static void TestQueueFlush(GDX12Device& device)
{
    auto* queue = device.GetCommandQueue();
    for (bool submit : {false, true})
    {
        if (submit) queue->ExecuteCommandList(queue->GetCommandList());
        queue->Flush();
        ComPtr<ID3D12Fence> gate;
        ThrowIfFailed(device.GetDevice()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)));
        // SDK work can enter the native queue after the engine's last submission.
        ThrowIfFailed(queue->GetCommandQueue()->Wait(gate.Get(), 1));
        std::promise<void> started, finished;
        auto startedFuture = started.get_future();
        auto finishedFuture = finished.get_future();
        std::thread flush([&]
        {
            started.set_value();
            try { queue->Flush(); finished.set_value(); }
            catch (...) { finished.set_exception(std::current_exception()); }
        });
        startedFuture.wait();
        const bool returnedEarly = finishedFuture.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready;
        const auto release = gate->Signal(1);
        flush.join();
        ThrowIfFailed(release);
        finishedFuture.get();
        queue->CPUWaitForFenceValue(queue->FenceValue);
        Require(!returnedEarly, "Flush returned while native queue work was still blocked");
    }
    std::cout << "Queue Flush waits for native work before and after engine submissions passed.\n";
}

static void TestBuffers(GDX12Device& device)
{
    const auto desc = CD3DX12_RESOURCE_DESC::Buffer(65536);
    std::vector<GDX12ResourceHandle> batch;
    for (UINT i = 0; i < 32; ++i)
        batch.push_back(device.CreateResource(D3D12_HEAP_TYPE_UPLOAD, desc, D3D12_RESOURCE_STATE_GENERIC_READ));
    const auto stats = Statistics(device);
    std::cout << "Small buffers: " << stats.AllocationCount << " allocations, " << stats.BlockCount
        << " blocks, " << stats.AllocationBytes << " allocated bytes, " << stats.BlockBytes << " block bytes\n";
    Require(stats.AllocationCount == 32 && stats.BlockCount < stats.AllocationCount,
        "Small buffers were not suballocated");
    Require(batch[0].GetAllocation()->GetHeap() != nullptr, "Expected a placed allocation");
    auto upload = batch[0];
    batch.clear();
    Require(Statistics(device).AllocationCount == 1, "Copied resource lost or leaked its allocation");

    BYTE* mapped = nullptr;
    ThrowIfFailed(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)));
    for (UINT i = 0; i < 65536; ++i) mapped[i] = static_cast<BYTE>(i * 17);
    upload->Unmap(0, nullptr);
    auto gpu = device.CreateResource(D3D12_HEAP_TYPE_DEFAULT, desc, D3D12_RESOURCE_STATE_COPY_DEST);
    auto readback = device.CreateResource(D3D12_HEAP_TYPE_READBACK, desc, D3D12_RESOURCE_STATE_COPY_DEST);
    auto* queue = device.GetCommandQueue();
    auto* commands = queue->GetCommandList();
    commands->GetCommandList()->CopyResource(gpu.Get(), upload.Get());
    const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(gpu.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    commands->GetCommandList()->ResourceBarrier(1, &barrier);
    commands->GetCommandList()->CopyResource(readback.Get(), gpu.Get());
    batch = { upload, gpu };
    upload.Reset();
    gpu.Reset();
    queue->ExecuteCommandList(commands);
    queue->Flush();
    batch.clear();
    ThrowIfFailed(readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped)));
    for (UINT i = 0; i < 65536; ++i)
        Require(mapped[i] == static_cast<BYTE>(i * 17), "GPU copy corrupted suballocated memory");
    readback->Unmap(0, nullptr);
    readback.Reset();

    // Poison freed upload regions before testing initialization and growth.
    {
        GDX12UploadBuffer<UINT> old(&device, 256);
        ThrowIfFailed(old.GetResource().D3DResource->Map(0, nullptr, reinterpret_cast<void**>(&mapped)));
        std::memset(mapped, 0xcd, 65536);
        old.GetResource().D3DResource->Unmap(0, nullptr);
    }
    GDX12UploadBuffer<UINT> growing(&device, 256);
    growing.CopyData(0, 0x12345678u);
    growing.Resize(512);
    auto& buffer = growing.GetResource().D3DResource;
    ThrowIfFailed(buffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped)));
    Require(*reinterpret_cast<UINT*>(mapped) == 0x12345678u, "Resize lost upload contents");
    for (UINT i = sizeof(UINT); i < 512 * 256; ++i)
        Require(mapped[i] == 0, "Recycled upload memory or new capacity was not initialized");
    buffer->Unmap(0, nullptr);
}

static void TestTrackedDefaults(GDX12Device& device)
{
    const GDX12TransformConstants identity;
    for (bool aligned : { false, true })
    {
        GDX12UploadBuffer<GDX12TransformConstants> buffer(&device, 0, EBufferType::Upload, aligned);
        buffer.TrackChanges();
        for (UINT count : { 1u, 2u })
        {
            buffer.Resize(count);
            buffer.CopyData(count - 1, identity);
            BYTE* mapped = nullptr;
            ThrowIfFailed(buffer.GetResource().D3DResource->Map(0, nullptr, reinterpret_cast<void**>(&mapped)));
            bool matches = true;
            for (UINT slot = 0; slot < count; ++slot)
                matches &= std::memcmp(mapped + slot * buffer.GetElementSize(), &identity, sizeof(identity)) == 0;
            buffer.GetResource().D3DResource->Unmap(0, nullptr);
            Require(matches, "Tracked nonzero defaults were skipped on initial allocation or growth");
        }
        const auto revision = buffer.GetRevision();
        buffer.CopyData(0, identity);
        Require(buffer.GetRevision() == revision, "Unchanged tracked data lost its no-change signal");

        GDX12UploadBuffer<GDX12TransformConstants> late(&device, 1, EBufferType::Upload, aligned);
        auto translated = identity;
        translated.WorldMatrix._41 = 17;
        late.CopyData(0, translated);
        late.TrackChanges();
        Require(std::memcmp(late.GetCPUData(), &translated, sizeof(translated)) == 0,
            "Enabling tracking lost already uploaded values");
    }
    std::cout << "Tracked upload mirrors: nonzero defaults, growth, alignment and late enable passed.\n";
}

static void TestTextures(GDX12Device& device)
{
    GDX12DeviceResources resources;
    resources.Initialize(&device);
    GDX12Texture texture;
    GDX12RenderHelpers::InitializeRenderTarget(texture, resources, 64, 64, DXGI_FORMAT_R8G8B8A8_UNORM);
    const auto srvIndex = texture.GetSRV()->HeapIndex;
    const auto rtvIndex = texture.GetRTV()->HeapIndex;
    auto retained = texture.GetResource()->D3DResource;
    const auto count = Statistics(device).AllocationCount;
    texture.Resize(128, 128);
    Require(Statistics(device).AllocationCount == count + 1, "Resize freed a retained old allocation");
    Require(retained->GetDesc().Width == 64 && texture.GetResource()->D3DResource->GetDesc().Width == 128,
        "Texture resize did not replace the resource");
    Require(texture.GetSRV()->HeapIndex == srvIndex && texture.GetRTV()->HeapIndex == rtvIndex,
        "Texture resize changed descriptor slots");
    retained.Reset();
    Require(Statistics(device).AllocationCount == count, "Resize leaked an allocation");

    GDX12TextureDesc imported;
    imported.ExternalResource = texture.GetResource()->D3DResource;
    auto copied = imported;
    imported.ExternalResource.Reset();
    Require(copied.ExternalResource.GetAllocation() == texture.GetResource()->D3DResource.GetAllocation(),
        "Texture description copy did not retain allocation ownership");
}

int main()
{
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    try
    {
        TestApp app;
        ComPtr<ID3D12Debug> debug;
        ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
        debug->EnableDebugLayer();
        GDX12Device device;
        ThrowIfFailed(device.Initialize(GDX12DeviceFactory::GetMostPerformantAdapter()));
        ComPtr<ID3D12InfoQueue> info;
        ThrowIfFailed(device.GetDevice().As(&info));
        info->ClearStoredMessages();
        TestQueueFlush(device);
        TestBuffers(device);
        TestTrackedDefaults(device);
        Require(Statistics(device).AllocationCount == 0, "Buffers leaked allocations");
        TestTextures(device);
        Require(Statistics(device).AllocationCount == 0, "Textures leaked allocations");

        auto survivor = device.CreateResource(D3D12_HEAP_TYPE_UPLOAD, CD3DX12_RESOURCE_DESC::Buffer(256),
            D3D12_RESOURCE_STATE_GENERIC_READ);
        device.GetCommandQueue()->Flush();
        device.Reset();
        auto copy = survivor;
        survivor.Reset();
        void* mapped = nullptr;
        ThrowIfFailed(copy->Map(0, nullptr, &mapped));
        std::memset(mapped, 0, 256);
        copy->Unmap(0, nullptr);
        copy.Reset();
        for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i)
        {
            SIZE_T size = 0;
            ThrowIfFailed(info->GetMessage(i, nullptr, &size));
            std::vector<BYTE> storage(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            ThrowIfFailed(info->GetMessage(i, message, &size));
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
                throw std::runtime_error(message->pDescription);
        }
        std::cout << "D3D12MA: suballocation, GPU copies, upload initialization, resize, ownership and teardown passed.\n";
        return 0;
    }
    catch (const DxException& error) { std::wcerr << error.ToString() << '\n'; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
    return 1;
}

#include "Rendering/Water/GDX12WaterOffload.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12CommandQueue.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include "Engine.Core/Logging/Logger.h"
#include <chrono>
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace
{
    struct Handle { HANDLE Value = nullptr; ~Handle() { if (Value) CloseHandle(Value); } };
    template<class T> ComPtr<T> Open(GDX12Device& owner, T* object, GDX12Device& other)
    {
        Handle handle;
        ThrowIfFailed(owner.GetDevice()->CreateSharedHandle(object, nullptr, GENERIC_ALL, nullptr, &handle.Value));
        ComPtr<T> imported;
        ThrowIfFailed(other.GetDevice()->OpenSharedHandle(handle.Value, IID_PPV_ARGS(&imported)));
        return imported;
    }
    uint64_t Completed(ID3D12Fence* fence)
    {
        const auto value = fence->GetCompletedValue();
        if (value == UINT64_MAX) ThrowIfFailed(DXGI_ERROR_DEVICE_REMOVED);
        return value;
    }
    template<class T> auto Buffer(GDX12Device* device, GDX12DescriptorHeap* heap, UINT count)
    {
        auto b = std::make_unique<GDX12UploadBuffer<T>>(device, count, EBufferType::Default, false);
        b->CreateSRV(heap, heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength));
        b->CreateUAV(heap, heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength));
        return b;
    }
    template<class T> std::vector<T> Read(GDX12Device& device, GDX12UploadBuffer<T>& buffer)
    {
        auto* queue = device.GetCommandQueue();
        const auto bytes = uint64_t(buffer.GetElementCount()) * sizeof(T);
        auto readback = device.CreateResource(D3D12_HEAP_TYPE_READBACK,
            CD3DX12_RESOURCE_DESC::Buffer(bytes), D3D12_RESOURCE_STATE_COPY_DEST);
        auto& resource = buffer.GetResource();
        const auto prior = resource.GetCurrentState();
        auto* commands = queue->GetCommandList();
        commands->ResourceBarrier({resource.GetCopySourceBarrier()});
        commands->GetCommandList()->CopyBufferRegion(readback.Get(), 0, resource.D3DResource.Get(), 0, bytes);
        commands->ResourceBarrier({resource.GetTransitionBarrier(prior)});
        queue->ExecuteCommandList(commands);
        queue->CPUWaitForFenceValue(queue->FenceValue);
        void* mapped = nullptr;
        D3D12_RANGE range{0, static_cast<SIZE_T>(bytes)};
        ThrowIfFailed(readback->Map(0, &range, &mapped));
        std::vector<T> result(buffer.GetElementCount());
        std::memcpy(result.data(), mapped, static_cast<size_t>(bytes));
        const D3D12_RANGE written{0, 0};
        readback->Unmap(0, &written);
        return result;
    }
}

GDX12WaterOffload::GDX12WaterOffload(GDX12Device* primary, GDX12DescriptorHeap* primaryHeap,
    ComPtr<IDXGIAdapter4> secondaryAdapter, const GDX12FluidInstance& seed) : _primary(primary), _displayHeap(primaryHeap), _input(seed)
{
    if (!primary || !primaryHeap || !secondaryAdapter) throw std::invalid_argument("Water offload requires two adapters");
    DXGI_ADAPTER_DESC1 desc{};
    ThrowIfFailed(secondaryAdapter->GetDesc1(&desc));
    const auto luid = primary->GetAdapterLuid();
    if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ||
        (desc.AdapterLuid.HighPart == luid.HighPart && desc.AdapterLuid.LowPart == luid.LowPart))
        throw std::invalid_argument("Water offload secondary must be a distinct hardware adapter");
    ThrowIfFailed(_secondary.Initialize(secondaryAdapter));
    ThrowIfFailed(_secondary.GetCommandQueue()->GetCommandQueue()->SetName(L"Water secondary solver queue"));
    _heap = std::make_unique<GDX12DescriptorHeap>(&_secondary, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        512, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
    _solver.Initialize(&_secondary);
    _buffers = GDX12FluidSolver::CreateBuffers(&_secondary, _heap.get(), seed);
    _constants = std::make_unique<GDX12UploadBuffer<GDX12FluidParameters>>(&_secondary, 1);
    _packed = Buffer<XMFLOAT3>(&_secondary, _heap.get(), _buffers.Count);
    _received = Buffer<XMFLOAT3>(primary, primaryHeap, _buffers.Count);
    _display = Buffer<GDX12FluidSolver::Particle>(primary, primaryHeap, _buffers.Count);
    _seed = std::make_unique<GDX12UploadBuffer<GDX12FluidSolver::Particle>>(primary, _buffers.Count, EBufferType::Upload, false);
    const auto particles = GDX12FluidSolver::MakeSeed(seed);
    _seed->CopyData(0, particles.data(), _buffers.Count);
    GDX12RootSignatureDesc root;
    root.VolatileSRVData = true;
    root.Constants = {2}; root.NumSingleSRVSlots = 2; root.NumSingleUAVSlots = 2;
    _packRoot = std::make_unique<GDX12RootSignature>(&_secondary, root);
    _displayRoot = std::make_unique<GDX12RootSignature>(primary, root);
    _packPSO = GDX12RenderHelpers::CreateComputePipeline(&_secondary, _packRoot.get(), APP_SHADERS_FOLDER "WaterTransfer.hlsl", "Pack");
    _displayPSO = GDX12RenderHelpers::CreateComputePipeline(primary, _displayRoot.get(), APP_SHADERS_FOLDER "WaterTransfer.hlsl", "AdvanceDisplay");
    const auto flags = D3D12_FENCE_FLAG_SHARED | D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER;
    ThrowIfFailed(_secondary.GetDevice()->CreateFence(0, flags, IID_PPV_ARGS(&_readySecondary)));
    _readyPrimary = Open(_secondary, _readySecondary.Get(), *primary);
    ThrowIfFailed(primary->GetDevice()->CreateFence(0, flags, IID_PPV_ARGS(&_freePrimary)));
    _freeSecondary = Open(*primary, _freePrimary.Get(), _secondary);
    const CD3DX12_HEAP_PROPERTIES properties(D3D12_HEAP_TYPE_DEFAULT);
    const auto resource = CD3DX12_RESOURCE_DESC::Buffer(GetPayloadBytes(), D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER);
    const auto secondaryInfo = _secondary.GetDevice()->GetResourceAllocationInfo(0, 1, &resource);
    const auto primaryInfo = primary->GetDevice()->GetResourceAllocationInfo(0, 1, &resource);
    D3D12_HEAP_DESC heap{};
    heap.SizeInBytes = (std::max)(secondaryInfo.SizeInBytes, primaryInfo.SizeInBytes);
    heap.Alignment = (std::max)(secondaryInfo.Alignment, primaryInfo.Alignment);
    heap.Properties = properties;
    heap.Flags = D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER;
    for (auto& slot : _slots)
    {
        ThrowIfFailed(_secondary.GetDevice()->CreateHeap(&heap, IID_PPV_ARGS(&slot.SecondaryHeap)));
        slot.PrimaryHeap = Open(_secondary, slot.SecondaryHeap.Get(), *primary);
        ThrowIfFailed(_secondary.GetDevice()->CreatePlacedResource(slot.SecondaryHeap.Get(), 0, &resource,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&slot.Secondary)));
        ThrowIfFailed(primary->GetDevice()->CreatePlacedResource(slot.PrimaryHeap.Get(), 0, &resource,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&slot.Primary)));
    }
    auto* commands = primary->GetCommandQueue()->GetCommandList();
    commands->ResourceBarrier({_display->GetResource().GetCopyDestBarrier()});
    commands->CopyResource(_display->GetResource().D3DResource.Get(), _seed->GetResource().D3DResource.Get());
    commands->ResourceBarrier({_display->GetResource().GetSRVBarrier()});
    primary->GetCommandQueue()->ExecuteCommandList(commands);
    Log::Info("Water", "Offload ", _buffers.Count, " particles: ", primary->GetDeviceFeatures().Name,
        " <- ", _secondary.GetDeviceFeatures().Name, "; position payload ", GetPayloadBytes(), " bytes");
}

GDX12WaterOffload::~GDX12WaterOffload()
{
    Stop();
    // Destruction/reset may wait; steady-state rendering never waits for secondary work.
    try
    {
        _primary->GetCommandQueue()->Flush();
        _secondary.GetCommandQueue()->Flush();
        Log::Info("Water", "Offload stopped: executed steps=", _steps, ", published results=", _sequence,
            ", last imported=", _last.Sequence, ", payload bytes/result=", GetPayloadBytes());
    }
    catch (...) { OutputDebugStringA("Water offload drain failed during shutdown\n"); }
}

void GDX12WaterOffload::Simulate(const GDX12FluidInstance& input, GDX12FluidSolver::Steps steps)
{
    ValidateFluidInstance(input);
    Slot* target = nullptr;
    {
        const auto released = Completed(_freeSecondary.Get());
        std::lock_guard<std::mutex> guard(_mutex);
        for (auto& slot : _slots) if (!slot.Published && released >= slot.Produced.Sequence) { target = &slot; break; }
    }
    auto* queue = _secondary.GetCommandQueue();
    auto* commands = queue->GetCommandList();
    {
        GDX12_SCOPED_EVENT(commands, "Water secondary solver + position export", Colors::Aqua);
        commands->SetDescriptorHeaps({_heap.get()});
        _constants->CopyData(0, MakeFluidParameters(input.ContainerToWorld, input.Component.GetSettings()));
        _head = _solver.Record(*commands, _buffers, _head, input.Component.GetSettings(), steps, _constants->GetElementAddress(0));
        if (target)
        {
            auto& current = *_buffers.Particles[_head.Current];
            commands->ResourceBarrier({current.GetResource().GetNonPixelShaderResourceBarrier(), _packed->GetResource().GetUnorderedAccessBarrier()});
            commands->SetComputeRootSignature(_packRoot.get());
            const UINT constants[] = {_buffers.Count, 0};
            commands->GetCommandList()->SetComputeRoot32BitConstants(0, 2, constants, 0);
            commands->SetComputeSRV(0, current.GetSRV()->GPUHandle);
            commands->SetComputeUAV(0, _packed->GetUAV()->GPUHandle);
            commands->SetPipelineState(_packPSO);
            commands->Dispatch((_buffers.Count + 127) / 128, 1, 1);
            commands->ResourceBarrier({_packed->GetResource().GetCopySourceBarrier(),
                CD3DX12_RESOURCE_BARRIER::Transition(target->Secondary.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST)});
            commands->GetCommandList()->CopyBufferRegion(target->Secondary.Get(), 0, _packed->GetResource().D3DResource.Get(), 0, GetPayloadBytes());
            commands->ResourceBarrier({CD3DX12_RESOURCE_BARRIER::Transition(target->Secondary.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON)});
        }
    }
    if (target && target->Produced.Sequence)
        ThrowIfFailed(queue->GetCommandQueue()->Wait(_freeSecondary.Get(), target->Produced.Sequence));
    queue->ExecuteCommandList(commands);
    const auto sequence = target ? ++_sequence : 0;
    if (target) ThrowIfFailed(queue->GetCommandQueue()->Signal(_readySecondary.Get(), sequence));
    // Only this worker waits, allowing immutable constants and bounded secondary submissions.
    queue->CPUWaitForFenceValue(queue->Signal());
    _steps += steps.Count;
    if (target)
    {
        std::lock_guard<std::mutex> guard(_mutex);
        target->Produced = {sequence, _steps, steps, input};
        target->Published = true;
    }
}

bool GDX12WaterOffload::UpdateDisplay()
{
    std::lock_guard<std::mutex> guard(_mutex);
    if (_failure) std::rethrow_exception(_failure);
    const auto ready = Completed(_readyPrimary.Get());
    Slot* source = nullptr;
    for (auto& slot : _slots)
        if (slot.Published && slot.Produced.Sequence <= ready &&
            (!source || slot.Produced.Sequence < source->Produced.Sequence)) source = &slot;
    auto* queue = _primary->GetCommandQueue();
    auto* commands = queue->GetCommandList();
    {
        GDX12_SCOPED_EVENT(commands, "Water ready import + displayed history", Colors::Aqua);
        if (source)
        {
            ThrowIfFailed(queue->GetCommandQueue()->Wait(_readyPrimary.Get(), source->Produced.Sequence));
            commands->ResourceBarrier({_received->GetResource().GetCopyDestBarrier(),
                CD3DX12_RESOURCE_BARRIER::Transition(source->Primary.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE)});
            commands->GetCommandList()->CopyBufferRegion(_received->GetResource().D3DResource.Get(), 0, source->Primary.Get(), 0, GetPayloadBytes());
            commands->ResourceBarrier({CD3DX12_RESOURCE_BARRIER::Transition(source->Primary.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON)});
        }
        commands->SetDescriptorHeaps({_displayHeap});
        commands->ResourceBarrier({_received->GetResource().GetNonPixelShaderResourceBarrier(),
            _display->GetResource().GetUnorderedAccessBarrier()});
        commands->SetComputeRootSignature(_displayRoot.get());
        const UINT constants[] = {_buffers.Count, source ? 1u : 0u};
        commands->GetCommandList()->SetComputeRoot32BitConstants(0, 2, constants, 0);
        commands->SetComputeSRV(1, _received->GetSRV()->GPUHandle);
        commands->SetComputeUAV(1, _display->GetUAV()->GPUHandle);
        commands->SetPipelineState(_displayPSO);
        commands->Dispatch((_buffers.Count + 127) / 128, 1, 1);
        commands->ResourceBarrier({_display->GetResource().GetSRVBarrier()});
    }
    queue->ExecuteCommandList(commands);
    if (source)
    {
        ThrowIfFailed(queue->GetCommandQueue()->Signal(_freePrimary.Get(), source->Produced.Sequence));
        _last = source->Produced;
        source->Published = false;
        _wake.notify_one();
    }
    return source != nullptr;
}

GDX12WaterOffload::Result GDX12WaterOffload::LastResult() const
{
    std::lock_guard<std::mutex> guard(_mutex);
    return _last;
}

void GDX12WaterOffload::Update(const GDX12FluidInstance& input)
{
    ValidateFluidInstance(input);
    std::lock_guard<std::mutex> guard(_mutex);
    if (_failure) std::rethrow_exception(_failure);
    _input = input;
    _wake.notify_one();
}

void GDX12WaterOffload::Start()
{
    if (_worker.joinable()) throw std::logic_error("Water worker already started");
    _stop = false;
    _worker = std::thread([this] { Run(); });
}

void GDX12WaterOffload::Stop()
{
    { std::lock_guard<std::mutex> guard(_mutex); _stop = true; }
    _wake.notify_one();
    if (_worker.joinable()) _worker.join();
}

void GDX12WaterOffload::Run()
{
    try
    {
        auto previous = std::chrono::steady_clock::now();
        for (;;)
        {
            GDX12FluidInstance input;
            {
                std::unique_lock<std::mutex> guard(_mutex);
                if (_stop) return;
                input = _input;
                if (!input.Component.GetSettings().SimulationEnabled || input.DeltaSeconds == 0 || input.ClockScale == 0 ||
                    input.Component.GetSettings().TimeScale == 0)
                {
                    _wake.wait_for(guard, std::chrono::milliseconds(1));
                    previous = std::chrono::steady_clock::now();
                    continue;
                }
            }
            const auto now = std::chrono::steady_clock::now();
            const auto elapsed = std::chrono::duration<float>(now - previous).count();
            previous = now;
            const auto steps = GDX12FluidSolver::PlanSteps(input.Component.GetSettings(),
                elapsed * input.ClockScale * input.Component.GetSettings().TimeScale);
            Simulate(input, steps);
        }
    }
    catch (...)
    {
        std::lock_guard<std::mutex> guard(_mutex);
        _failure = std::current_exception();
    }
}

std::vector<GDX12FluidSolver::Particle> GDX12WaterOffload::ReadSolverState()
{
    if (_worker.joinable()) throw std::logic_error("Stop water worker before diagnostic readback");
    return Read(_secondary, *_buffers.Particles[_head.Current]);
}

std::vector<XMFLOAT3> GDX12WaterOffload::ReadSentPositions()
{
    if (_worker.joinable()) throw std::logic_error("Stop water worker before diagnostic readback");
    return Read(_secondary, *_packed);
}

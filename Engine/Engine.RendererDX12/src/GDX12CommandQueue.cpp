#include "Engine.RendererDX12/GDX12CommandQueue.h"

#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12StreamlineSDK.h"

#include <limits>
#include <stdexcept>

GDX12CommandQueue::GDX12CommandQueue(GDX12Device* device, D3D12_COMMAND_LIST_TYPE type)
    : _device(device), _type(type), _lastSubmittedFenceValue(0)
{
    if (!device || (type != D3D12_COMMAND_LIST_TYPE_DIRECT && type != D3D12_COMMAND_LIST_TYPE_COPY))
    {
        throw std::invalid_argument("ERROR: Given command queue has no device or uses a type other than DIRECT or COPY. Supply an initialized "
            "device and D3D12_COMMAND_LIST_TYPE_DIRECT or D3D12_COMMAND_LIST_TYPE_COPY.");
    }

    D3D12_COMMAND_QUEUE_DESC desc = {};
    desc.Type = type;
    desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    desc.NodeMask = 0;

    ThrowIfFailed(device->GetCommandQueueCreationDevice()->CreateCommandQueue(&desc, IID_PPV_ARGS(&_proxyCommandQueue)));
    _commandQueue = GDX12StreamlineSDK::Get().GetNativeCommandQueue(_proxyCommandQueue);
    ThrowIfFailed(device->GetDevice()->CreateFence(0,
        D3D12_FENCE_FLAG_SHARED | D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER, IID_PPV_ARGS(&_fence)));
}

void GDX12CommandQueue::Reset()
{
    if (_commandQueue && _fence) { Flush(); }
    _workingCommandLists.clear();
    _availableCommandLists.clear();
    _openedFences.clear();
    _proxyCommandQueue.Reset();
    _commandQueue.Reset();
    _fence.Reset();
    _lastSubmittedFenceValue = 0;
}

GDX12CommandQueue::~GDX12CommandQueue()
{
    try { Reset(); }
    catch (const DxException& error) { OutputDebugStringW(error.ToString().c_str()); }
    catch (const std::exception& error) { OutputDebugStringA(error.what()); }
}

const ComPtr<ID3D12CommandQueue>& GDX12CommandQueue::GetCommandQueue()
{
    return _commandQueue;
}

const ComPtr<ID3D12CommandQueue>& GDX12CommandQueue::GetPresentCommandQueue()
{
    return _proxyCommandQueue ? _proxyCommandQueue : _commandQueue;
}

GDX12CommandList* GDX12CommandQueue::GetCommandList()
{
    GDX12CommandList* rawPtr;
    ClearCompletedLists();

    if (!_availableCommandLists.empty())
    {
        auto commandList = std::move(_availableCommandLists.back());
        _availableCommandLists.pop_back();
        commandList->Reset();
        rawPtr = commandList.get();
        _workingCommandLists.push_back(std::move(commandList));
        return rawPtr;
    }

    auto commandList = std::make_unique<GDX12CommandList>(_device, _type);
    rawPtr = commandList.get();
    _workingCommandLists.push_back(std::move(commandList));
    return rawPtr;
}

void GDX12CommandQueue::ExecuteCommandList(GDX12CommandList* commandList)
{
    if (!commandList || commandList->GetType() != _type ||
        commandList->FenceValue != (std::numeric_limits<UINT64>::max)())
    {
        throw std::invalid_argument("ERROR: Given command list is not recording or its type differs from the submission queue. Record the command "
            "list using the same queue and command-list type that will submit it.");
    }
    bool owned = false;
    for (auto& list : _workingCommandLists) { if (list.get() == commandList) { owned = true; break; } }
    if (!owned) { throw std::invalid_argument("ERROR: Command list belongs to another queue. Submit the command list through the queue that created it."); }

    ThrowIfFailed(commandList->GetCommandList()->Close());
    ID3D12CommandList* ppLists[] = { commandList->GetCommandList().Get() };
    _commandQueue->ExecuteCommandLists(1, ppLists);
    commandList->FenceValue = Signal().Value;
}

void GDX12CommandQueue::ExecuteCommandLists(std::vector<GDX12CommandList*>& commandLists)
{
    if (commandLists.empty()) { return; }
    if (commandLists.size() > (std::numeric_limits<UINT>::max)())
    {
        throw std::overflow_error("ERROR: Given submission contains more than UINT_MAX command lists. Split the submission into batches "
            "containing no more than UINT_MAX command lists.");
    }
    std::vector<ID3D12CommandList*> ppLists;
    UINT commandListCount = static_cast<UINT>(commandLists.size());
    ppLists.reserve(commandListCount);

    for (UINT i = 0; i < commandListCount; ++i)
    {
        auto* commandList = commandLists[i];
        if (!commandList || commandList->GetType() != _type ||
            commandList->FenceValue != (std::numeric_limits<UINT64>::max)())
        {
            throw std::invalid_argument("ERROR: Given command list is not recording or its type differs from the submission queue. Record the command "
                "list using the same queue and command-list type that will submit it.");
        }
        bool owned = false;
        for (auto& list : _workingCommandLists) { if (list.get() == commandList) { owned = true; break; } }
        if (!owned) { throw std::invalid_argument("ERROR: Command list belongs to another queue. Submit the command list through the queue that created it."); }
        for (UINT previous = 0; previous < i; ++previous)
        {
            if (commandLists[previous] == commandList)
            {
                throw std::invalid_argument("ERROR: Command list submitted twice in one batch. Include each command list at most once in a submission "
                    "batch.");
            }
        }
        ppLists.push_back(commandLists[i]->GetCommandList().Get());
    }

    for (auto* commandList : commandLists) { ThrowIfFailed(commandList->GetCommandList()->Close()); }

    _commandQueue->ExecuteCommandLists(commandListCount, ppLists.data());
    auto point = Signal();
    for (auto* commandList : commandLists) { commandList->FenceValue = point.Value; }
}

D3D12_COMMAND_LIST_TYPE GDX12CommandQueue::GetType() const
{
    return _type;
}

GDX12CompletionPoint GDX12CommandQueue::Signal()
{
    if (_lastSubmittedFenceValue >= (std::numeric_limits<UINT64>::max)() - 1)
    {
        throw std::overflow_error("ERROR: Command queue fence counter exhausted. Recreate the command queue and its fence before submitting more "
            "work.");
    }
    const UINT64 value = _lastSubmittedFenceValue + 1;
    ThrowIfFailed(_commandQueue->Signal(_fence.Get(), value));
    _lastSubmittedFenceValue = value;
    return GetCompletionPoint();
}

GDX12CompletionPoint GDX12CommandQueue::GetCompletionPoint() const
{
    return { _device, const_cast<GDX12CommandQueue*>(this), _fence, _lastSubmittedFenceValue };
}

void GDX12CommandQueue::GPUWait(const GDX12CompletionPoint& point)
{
    if (point.Value == 0) { return; }
    if (!point.Device || !point.Queue || !point.Fence ||
        point.Value == (std::numeric_limits<UINT64>::max)())
    {
        throw std::invalid_argument("ERROR: Given queue completion point has invalid device, queue, fence, or signal-value fields. Use a "
            "completion point returned by a valid command-queue submission or signal.");
    }
    if (point.Queue->_device != point.Device || point.Queue->_fence.Get() != point.Fence.Get() ||
        point.Value > point.Queue->_lastSubmittedFenceValue)
    {
        throw std::invalid_argument("ERROR: Given completion point does not match a submitted signal of its source queue. Use a completion point "
            "naming this queue device, fence, and an already submitted signal value.");
    }
    if (point.Fence.Get() == _fence.Get())
    {
        if (point.Value > _lastSubmittedFenceValue)
        {
            throw std::invalid_argument("ERROR: Command queue cannot wait for its own future signal. Wait for an already submitted signal instead of a "
                "future value on the same queue.");
        }
        return;
    }

    ComPtr<ID3D12Fence> localFence = point.Fence;
    if (point.Device != _device)
    {
        localFence.Reset();
        for (const auto& opened : _openedFences)
        {
            if (opened.Source.Get() == point.Fence.Get()) { localFence = opened.Local; break; }
        }
        if (!localFence)
        {
            HANDLE handle = nullptr;
            ThrowIfFailed(point.Device->GetDevice()->CreateSharedHandle(
                point.Fence.Get(), nullptr, GENERIC_ALL, nullptr, &handle));
            const HRESULT result = _device->GetDevice()->OpenSharedHandle(handle, IID_PPV_ARGS(&localFence));
            CloseHandle(handle);
            ThrowIfFailed(result);
            _openedFences.push_back({ point.Fence, localFence });
        }
    }
    ThrowIfFailed(_commandQueue->Wait(localFence.Get(), point.Value));
}

void GDX12CommandQueue::CPUwait(const GDX12CompletionPoint& point)
{
    if (point.Value == 0) { return; }
    if (!point.Fence || point.Value == (std::numeric_limits<UINT64>::max)())
    {
        throw std::invalid_argument("ERROR: Given CPU completion point has invalid device, queue, fence, or signal-value fields. Use a completion "
            "point returned by a valid command-queue submission or signal.");
    }
    UINT64 completed = point.Fence->GetCompletedValue();
    if (completed == (std::numeric_limits<UINT64>::max)())
    {
        throw std::runtime_error("ERROR: Device removed while waiting for queue completion. Check GetDeviceRemovedReason() and recreate the "
            "device and its FrameGraph.");
    }
    if (completed >= point.Value) { return; }

    HANDLE event = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
    if (!event) { ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError())); }
    const HRESULT result = point.Fence->SetEventOnCompletion(point.Value, event);
    if (FAILED(result)) { CloseHandle(event); ThrowIfFailed(result); }
    const DWORD waitResult = WaitForSingleObjectEx(event, INFINITE, FALSE);
    const DWORD waitError = waitResult == WAIT_FAILED ? GetLastError() : ERROR_GEN_FAILURE;
    CloseHandle(event);
    if (waitResult != WAIT_OBJECT_0) { ThrowIfFailed(HRESULT_FROM_WIN32(waitError)); }
    if (point.Fence->GetCompletedValue() == (std::numeric_limits<UINT64>::max)())
    {
        throw std::runtime_error("ERROR: Device removed while waiting for queue completion. Check GetDeviceRemovedReason() and recreate the "
            "device and its FrameGraph.");
    }
}

const ComPtr<ID3D12Fence>& GDX12CommandQueue::GetFence()
{
    return _fence;
}

void GDX12CommandQueue::CPUWaitForFenceValue(uint64_t fenceValue)
{
    if (fenceValue > _lastSubmittedFenceValue)
    {
        throw std::invalid_argument("ERROR: Given CPU wait references a fence value not yet submitted by this queue. Wait only for fence values "
            "already submitted by this queue.");
    }
    CPUwait({ _device, this, _fence, fenceValue });
}

void GDX12CommandQueue::Flush()
{
    CPUwait(Signal());
}

void GDX12CommandQueue::ClearCompletedLists()
{
    const UINT64 completed = _fence->GetCompletedValue();
    if (completed == (std::numeric_limits<UINT64>::max)())
    {
        throw std::runtime_error("ERROR: Device removed while recycling command lists. Check GetDeviceRemovedReason() and recreate the device "
            "and its command queues.");
    }
    auto it = _workingCommandLists.begin();
    while (it != _workingCommandLists.end())
    {
        if ((*it)->FenceValue != (std::numeric_limits<UINT64>::max)() && completed >= (*it)->FenceValue)
        {
            _availableCommandLists.push_back(std::move(*it));
            it = _workingCommandLists.erase(it);
        }
        else { ++it; }
    }
}

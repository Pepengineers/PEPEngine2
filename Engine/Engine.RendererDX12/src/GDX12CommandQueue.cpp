#include "Engine.RendererDX12/GDX12CommandQueue.h"

#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12StreamlineSDK.h"
#include <algorithm>
#include <exception>
#include <limits>
#include <stdexcept>

namespace
{
    struct ScopedHandle
    {
        HANDLE Value = nullptr;
        ~ScopedHandle() { if (Value) CloseHandle(Value); }
    };


}

GDX12CommandQueue::GDX12CommandQueue(GDX12Device* device, D3D12_COMMAND_LIST_TYPE type)
    : FenceValue(0), _type(type), _device(device)
{
    if (!device || !device->GetDevice()) throw std::invalid_argument("Queue requires a live device");
    D3D12_COMMAND_QUEUE_DESC desc = {};
    desc.Type = type;
    desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    if (type == D3D12_COMMAND_LIST_TYPE_DIRECT)
    {
        ThrowIfFailed(device->GetCommandQueueCreationDevice()->CreateCommandQueue(&desc, IID_PPV_ARGS(&_proxyCommandQueue)));
        _commandQueue = GDX12StreamlineSDK::Get().GetNativeCommandQueue(_proxyCommandQueue);
    }
    else ThrowIfFailed(device->GetDevice()->CreateCommandQueue(&desc, IID_PPV_ARGS(&_commandQueue)));
    ThrowIfFailed(_commandQueue->SetName(type == D3D12_COMMAND_LIST_TYPE_COPY ? L"COPY queue" :
        type == D3D12_COMMAND_LIST_TYPE_COMPUTE ? L"COMPUTE queue" : L"DIRECT queue"));
    ThrowIfFailed(device->GetDevice()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_fence)));
}

void GDX12CommandQueue::Reset()
{
    if (!_fence) return;
    // Device removal must never turn UINT64_MAX into successful retirement or an infinite CPU wait.
    const auto reason = _device->GetDevice()->GetDeviceRemovedReason();
    std::exception_ptr failure;
    try
    {
        ThrowIfFailed(reason);
        Flush();
    }
    catch (...) { failure = std::current_exception(); }
    _issuedValue = 0;
    FenceValue = 0;
    _workingCommandLists.clear();
    _availableCommandLists.clear();
    _fence.Reset();
    _proxyCommandQueue.Reset();
    _commandQueue.Reset();
    if (failure) std::rethrow_exception(failure);
}

GDX12CommandQueue::~GDX12CommandQueue()
{
    try { Reset(); }
    catch (const DxException& error) { OutputDebugStringW(error.ToString().c_str()); }
    catch (const std::exception& error) { OutputDebugStringA(error.what()); }
    catch (...) { OutputDebugStringA("GDX12CommandQueue shutdown failed; call Reset explicitly to observe errors.\n"); }
}

uint64_t GDX12CommandQueue::NextFenceValue() const
{
    if (!_fence) throw std::logic_error("Submission queue was reset");
    if (FenceValue >= (std::numeric_limits<uint64_t>::max)() - 1)
        throw std::overflow_error("Submission fence values exhausted");
    if (FenceValue != _issuedValue) throw std::logic_error("Submission fence counter was modified externally");
    return FenceValue + 1;
}

UINT64 GDX12CommandQueue::Signal()
{
    const auto next = NextFenceValue();
    ThrowIfFailed(_commandQueue->Signal(_fence.Get(), next));
    FenceValue = _issuedValue = next;
    return next;
}

const ComPtr<ID3D12CommandQueue>& GDX12CommandQueue::GetCommandQueue() { return _commandQueue; }
const ComPtr<ID3D12CommandQueue>& GDX12CommandQueue::GetPresentCommandQueue()
{
    return _proxyCommandQueue ? _proxyCommandQueue : _commandQueue;
}

GDX12CommandList* GDX12CommandQueue::GetCommandList()
{
    if (!_fence) throw std::logic_error("Submission queue was reset");
    CollectCompletedLists();
    std::unique_ptr<GDX12CommandList> commandList;
    if (!_availableCommandLists.empty())
    {
        commandList = std::move(_availableCommandLists.back());
        _availableCommandLists.pop_back();
        commandList->Reset();
    }
    else commandList = std::make_unique<GDX12CommandList>(_device, _type);
    commandList->_owner = this;
    auto* result = commandList.get();
    _workingCommandLists.push_back(std::move(commandList));
    return result;
}

void GDX12CommandQueue::ValidateCommandList(GDX12CommandList* list) const
{
    if (!list || list->GetType() != _type || list->_owner != this || list->_submitted)
        throw std::invalid_argument("Command list must be recording and owned by the submitting queue");
}

void GDX12CommandQueue::ExecuteCommandList(GDX12CommandList* commandList)
{
    std::vector<GDX12CommandList*> lists{ commandList };
    ExecuteCommandLists(lists);
}

void GDX12CommandQueue::ExecuteCommandLists(std::vector<GDX12CommandList*>& commandLists)
{
    GDX12_SCOPED_CPU_EVENT("Submit");
    if (commandLists.empty() || commandLists.size() > (std::numeric_limits<UINT>::max)())
        throw std::invalid_argument("Invalid submission list count");
    NextFenceValue(); // Fail before executing any GPU work on overflow.
    for (size_t i = 0; i < commandLists.size(); ++i)
    {
        ValidateCommandList(commandLists[i]);
        if (std::find(commandLists.begin(), commandLists.begin() + i, commandLists[i]) != commandLists.begin() + i)
            throw std::invalid_argument("Duplicate command list in submission");
    }
    std::vector<ID3D12CommandList*> native;
    native.reserve(commandLists.size());
    for (auto* list : commandLists)
    {
        ThrowIfFailed(list->GetCommandList()->Close());
        native.push_back(list->GetCommandList().Get());
    }
    _commandQueue->ExecuteCommandLists(static_cast<UINT>(native.size()), native.data());
    // A failed Signal must not let an allocator with already submitted work be recycled.
    for (auto* list : commandLists) list->_submitted = true;
    for (auto* list : commandLists)
        for (const auto& signal : list->_signals)
            ThrowIfFailed(_commandQueue->Signal(signal.first.Get(), signal.second));
    const auto point = Signal();
    for (auto* list : commandLists)
    {
        list->FenceValue = point;
        list->_retirement = point;
        list->_retirementFence = _fence;
    }
}

const ComPtr<ID3D12Fence>& GDX12CommandQueue::GetFence() { return _fence; }

void GDX12CommandQueue::CPUWaitForFenceValue(uint64_t fenceValue)
{
    if (!_fence || fenceValue > _issuedValue)
        throw std::invalid_argument("CPU wait requires an issued local fence value");
    auto completed = _fence->GetCompletedValue();
    if (completed == (std::numeric_limits<uint64_t>::max)()) ThrowIfFailed(DXGI_ERROR_DEVICE_REMOVED);
    if (completed >= fenceValue) return;
    ScopedHandle event;
    event.Value = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
    if (!event.Value) { ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError())); }
    ThrowIfFailed(_fence->SetEventOnCompletion(fenceValue, event.Value));
    const auto result = WaitForSingleObjectEx(event.Value, INFINITE, FALSE);
    if (result != WAIT_OBJECT_0) { ThrowIfFailed(result == WAIT_FAILED ? HRESULT_FROM_WIN32(GetLastError()) : E_FAIL); }
    if (_fence->GetCompletedValue() == (std::numeric_limits<uint64_t>::max)()) ThrowIfFailed(DXGI_ERROR_DEVICE_REMOVED);
}

void GDX12CommandQueue::WaitForQueue(GDX12CommandQueue& producer, uint64_t fenceValue)
{
    if (producer._device != _device || !producer._fence || !fenceValue || fenceValue > producer._issuedValue)
        throw std::invalid_argument("Queue wait requires an issued value from the same device");
    if (producer._fence->GetCompletedValue() == (std::numeric_limits<uint64_t>::max)())
        ThrowIfFailed(DXGI_ERROR_DEVICE_REMOVED);
    ThrowIfFailed(_commandQueue->Wait(producer._fence.Get(), fenceValue));
}

void GDX12CommandQueue::Flush()
{
    const auto point = Signal();
    CPUWaitForFenceValue(point);
}

void GDX12CommandQueue::CollectCompletedLists()
{
    const auto completed = _fence->GetCompletedValue();
    if (completed == (std::numeric_limits<uint64_t>::max)()) ThrowIfFailed(DXGI_ERROR_DEVICE_REMOVED);
    for (auto& list : _workingCommandLists)
    {
        if (list && list->_retirement && completed >= list->_retirement)
        {
            _availableCommandLists.push_back(std::move(list));
        }
    }
    _workingCommandLists.erase(std::remove(_workingCommandLists.begin(), _workingCommandLists.end(), nullptr),
        _workingCommandLists.end());
}

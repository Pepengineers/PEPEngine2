#include "Engine.RendererDX12/GDX12CommandQueue.h"

#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12StreamlineSDK.h"
#include <algorithm>
#include <atomic>
#include <exception>
#include <limits>
#include <stdexcept>

using namespace GDX12Submission;

namespace
{
    uint64_t NextIdentity()
    {
        static std::atomic<uint64_t> next{1};
        auto value = next.load();
        do
        {
            if (value == (std::numeric_limits<uint64_t>::max)())
                throw std::overflow_error("Submission queue/timeline identities exhausted");
        } while (!next.compare_exchange_weak(value, value + 1));
        return value;
    }

    struct ScopedHandle
    {
        HANDLE Value = nullptr;
        ~ScopedHandle() { if (Value) CloseHandle(Value); }
    };

    QueueKind Kind(D3D12_COMMAND_LIST_TYPE type)
    {
        switch (type)
        {
        case D3D12_COMMAND_LIST_TYPE_DIRECT: return QueueKind::Direct;
        case D3D12_COMMAND_LIST_TYPE_COMPUTE: return QueueKind::Compute;
        case D3D12_COMMAND_LIST_TYPE_COPY: return QueueKind::Copy;
        default: throw std::invalid_argument("Unsupported submission queue type");
        }
    }
}

GDX12CommandQueue::GDX12CommandQueue(GDX12Device* device, D3D12_COMMAND_LIST_TYPE type, bool crossAdapterFence)
    : FenceValue(0), _type(type), _device(device)
{
    const auto kind = Kind(type);
    if (!device || device->GetSubmissionAdapterId() == AdapterId::Invalid)
        throw std::invalid_argument("Submission queue requires a live device identity");
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
    ThrowIfFailed(device->GetDevice()->CreateFence(0, crossAdapterFence ?
        D3D12_FENCE_FLAG_SHARED | D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER : D3D12_FENCE_FLAG_NONE,
        IID_PPV_ARGS(&_fence)));
    _timeline = std::make_shared<TimelineState>();
    _timeline->Identity = { { { device->GetSubmissionAdapterId(), kind }, NextIdentity() }, NextIdentity() };
    _timeline->Fence = _fence;
    _timeline->CrossAdapter = crossAdapterFence;
    device->_submissionQueues.push_back(this);
}

void GDX12CommandQueue::Reset()
{
    if (!_timeline) return;
    // Device removal must never turn UINT64_MAX into successful retirement or an infinite CPU wait.
    const auto reason = _device->GetDevice()->GetDeviceRemovedReason();
    std::exception_ptr failure;
    try
    {
        ThrowIfFailed(reason);
        Flush();
    }
    catch (...) { failure = std::current_exception(); }
    _timeline->Active = false;
    auto& queues = _device->_submissionQueues;
    queues.erase(std::remove(queues.begin(), queues.end(), this), queues.end());
    _timeline.reset();
    _imports.clear();
    _lastCompletion = {};
    _otherTimeline = {};
    FenceValue = 0;
    _workingCommandLists.clear();
    _availableCommandLists.clear();
    _otherFence.Reset();
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

QueueId GDX12CommandQueue::GetIdentity() const
{
    return _timeline ? _timeline->Identity.Producer : QueueId{};
}

TimelineId GDX12CommandQueue::GetTimelineIdentity() const
{
    return _timeline ? _timeline->Identity : TimelineId{};
}

CompletionPoint GDX12CommandQueue::GetLastCompletionPoint() const { return _lastCompletion; }

uint64_t GDX12CommandQueue::NextFenceValue() const
{
    if (!_timeline || !_timeline->Active) throw std::logic_error("Submission queue was reset");
    if (FenceValue >= (std::numeric_limits<uint64_t>::max)() - 1)
        throw std::overflow_error("Submission fence values exhausted");
    if (FenceValue != _timeline->IssuedValue) throw std::logic_error("Submission fence counter was modified externally");
    return FenceValue + 1;
}

CompletionPoint GDX12CommandQueue::SignalCompletion()
{
    const auto next = NextFenceValue();
    ThrowIfFailed(_commandQueue->Signal(_fence.Get(), next));
    FenceValue = next;
    _timeline->IssuedValue = next;
    _lastCompletion = CompletionPoint(_timeline->Identity, next);
    return _lastCompletion;
}

FenceProgress GDX12CommandQueue::GetCompletionProgress() const
{
    return GetCompletionProgress(GetTimelineIdentity());
}

FenceProgress GDX12CommandQueue::GetCompletionProgress(TimelineId producer) const
{
    return { producer, ResolveTimeline(producer)->GetCompletedValue() };
}

ComPtr<ID3D12Fence> GDX12CommandQueue::ResolveTimeline(TimelineId producer) const
{
    if (!_timeline || !producer.IsValid()) throw std::invalid_argument("Invalid or reset submission timeline");
    if (auto* local = _device->FindSubmissionQueue(producer)) return local->_fence;
    for (const auto& imported : _imports)
    {
        auto state = imported.Producer.lock();
        if (state && state->Active && state->Identity == producer) return imported.Fence;
    }
    throw std::invalid_argument("Unknown, stale or unimported producer timeline");
}

TimelineId GDX12CommandQueue::ImportTimeline(GDX12CommandQueue& producer)
{
    if (!_timeline || !producer._timeline || !producer._timeline->Active)
        throw std::invalid_argument("Cannot import a reset submission timeline");
    if (&producer == this) throw std::invalid_argument("Cannot import the queue's own timeline");
    const auto identity = producer.GetTimelineIdentity();
    if (identity.Producer.Location.Adapter == GetIdentity().Location.Adapter) return identity;
    if (!producer._timeline->CrossAdapter)
        throw std::invalid_argument("Cross-adapter export requires an explicitly shared producer fence");
    for (const auto& imported : _imports)
        if (imported.Producer.lock() == producer._timeline) return identity;
    ScopedHandle handle;
    ThrowIfFailed(producer._device->GetDevice()->CreateSharedHandle(producer._fence.Get(),
        nullptr, GENERIC_ALL, nullptr, &handle.Value));
    ComPtr<ID3D12Fence> view;
    ThrowIfFailed(_device->GetDevice()->OpenSharedHandle(handle.Value, IID_PPV_ARGS(&view)));
    _imports.erase(std::remove_if(_imports.begin(), _imports.end(), [](const ImportedTimeline& imported)
        { auto state = imported.Producer.lock(); return !state || !state->Active; }), _imports.end());
    _imports.push_back({ producer._timeline, view });
    _otherTimeline = identity;
    _otherFence = view;
    return identity;
}

void GDX12CommandQueue::WaitFor(const CompletionPoint& producer)
{
    if (!producer.IsValid()) throw std::invalid_argument("Invalid completion point");
    auto fence = ResolveTimeline(producer.Timeline());
    // Tokens can only be queue-issued. Resolution also rejects timelines invalidated by reset.
    if (fence->GetCompletedValue() == (std::numeric_limits<uint64_t>::max)())
        ThrowIfFailed(DXGI_ERROR_DEVICE_REMOVED);
    ThrowIfFailed(_commandQueue->Wait(fence.Get(), producer.Value()));
}

const ComPtr<ID3D12CommandQueue>& GDX12CommandQueue::GetCommandQueue() { return _commandQueue; }
const ComPtr<ID3D12CommandQueue>& GDX12CommandQueue::GetPresentCommandQueue()
{
    return _proxyCommandQueue ? _proxyCommandQueue : _commandQueue;
}

GDX12CommandList* GDX12CommandQueue::GetCommandList()
{
    if (!_timeline) throw std::logic_error("Submission queue was reset");
    ClearCompletedLists();
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
    const auto point = SignalCompletion();
    for (auto* list : commandLists)
    {
        list->FenceValue = point.Value();
        list->_retirement = point;
        list->_retirementFence = _fence;
    }
}

const ComPtr<ID3D12Fence>& GDX12CommandQueue::GetFence() { return _fence; }
ComPtr<ID3D12Fence>& GDX12CommandQueue::GetOtherFence() { return _otherFence; }

void GDX12CommandQueue::CPUWaitForFenceValue(uint64_t fenceValue)
{
    if (!_timeline || fenceValue > _timeline->IssuedValue)
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

void GDX12CommandQueue::WaitForOtherFence(uint64_t otherFenceValue)
{
    auto fence = ResolveTimeline(_otherTimeline);
    if (fence.Get() != _otherFence.Get()) throw std::invalid_argument("Unverified legacy fence import");
    std::shared_ptr<TimelineState> state;
    for (const auto& imported : _imports)
    {
        auto candidate = imported.Producer.lock();
        if (candidate && candidate->Identity == _otherTimeline) state = std::move(candidate);
    }
    if (!state || !otherFenceValue || otherFenceValue > state->IssuedValue)
        throw std::invalid_argument("Legacy wait requires an issued producer value");
    WaitFor(CompletionPoint(_otherTimeline, otherFenceValue));
}

void GDX12CommandQueue::WaitForQueue(GDX12CommandQueue& producer, uint64_t fenceValue)
{
    if (!producer._timeline || !fenceValue || fenceValue > producer._timeline->IssuedValue)
        throw std::invalid_argument("Queue wait requires an issued producer value");
    WaitFor(CompletionPoint(producer.GetTimelineIdentity(), fenceValue));
}

void GDX12CommandQueue::Flush()
{
    const auto point = SignalCompletion();
    CPUWaitForFenceValue(point.Value());
}

void GDX12CommandQueue::ClearCompletedLists()
{
    const auto progress = GetCompletionProgress();
    if (progress.CompletedValue == (std::numeric_limits<uint64_t>::max)()) ThrowIfFailed(DXGI_ERROR_DEVICE_REMOVED);
    auto it = _workingCommandLists.begin();
    while (it != _workingCommandLists.end())
    {
        if ((*it)->_retirement.IsReachedBy(progress))
        {
            _availableCommandLists.push_back(std::move(*it));
            it = _workingCommandLists.erase(it);
        }
        else ++it;
    }
}

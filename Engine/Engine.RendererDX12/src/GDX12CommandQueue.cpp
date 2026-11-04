#include "Engine.RendererDX12/GDX12CommandQueue.h"

#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12StreamlineSDK.h"
#include <stdexcept>

GDX12CommandQueue::GDX12CommandQueue(GDX12Device* device, D3D12_COMMAND_LIST_TYPE type)
    : FenceValue(0), _type(type), _device(device)
{
    D3D12_COMMAND_QUEUE_DESC desc = {};
    desc.Type = type;
    desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    desc.NodeMask = 0;

    if (type == D3D12_COMMAND_LIST_TYPE_DIRECT)
    {
        ThrowIfFailed(device->GetCommandQueueCreationDevice()->CreateCommandQueue(&desc, IID_PPV_ARGS(&_proxyCommandQueue)));
        _commandQueue = GDX12StreamlineSDK::Get().GetNativeCommandQueue(_proxyCommandQueue);
    }
    else ThrowIfFailed(device->GetDevice()->CreateCommandQueue(&desc, IID_PPV_ARGS(&_commandQueue)));
    _commandQueue->SetName(type == D3D12_COMMAND_LIST_TYPE_COMPUTE ? L"COMPUTE queue" : L"DIRECT queue");
    ThrowIfFailed(device->GetDevice()->CreateFence(FenceValue, 
        type == D3D12_COMMAND_LIST_TYPE_DIRECT ? D3D12_FENCE_FLAG_SHARED | D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER :
        D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_fence)));
}

void GDX12CommandQueue::Reset()
{
    _proxyCommandQueue.Reset();
    _commandQueue.Reset();
    _fence.Reset();
    _workingCommandLists.clear();
    _availableCommandLists.clear();
    _otherFence.Reset();
}

GDX12CommandQueue::~GDX12CommandQueue()
{
    Reset();
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
    GDX12_SCOPED_CPU_EVENT("Submit");
    if (commandList->GetType() != _type) throw std::invalid_argument("Command list/queue type mismatch");
    ThrowIfFailed(commandList->GetCommandList()->Close());
    ID3D12CommandList* ppLists[] = { commandList->GetCommandList().Get() };
    _commandQueue->ExecuteCommandLists(1, ppLists);
    FenceValue++;
    ThrowIfFailed(_commandQueue->Signal(_fence.Get(), FenceValue));
    commandList->FenceValue = FenceValue;
}

void GDX12CommandQueue::ExecuteCommandLists(std::vector<GDX12CommandList*>& commandLists)
{
    std::vector<ID3D12CommandList*> ppLists;
    const UINT commandListCount = static_cast<UINT>(commandLists.size());
    ppLists.reserve(commandListCount);

    for (UINT i = 0; i < commandListCount; ++i)
    {
        if (commandLists[i]->GetType() != _type) throw std::invalid_argument("Command list/queue type mismatch");
        ThrowIfFailed(commandLists[i]->GetCommandList()->Close());
        ppLists.push_back(commandLists[i]->GetCommandList().Get());
    }

    _commandQueue->ExecuteCommandLists(commandListCount, ppLists.data());

    FenceValue++;
    ThrowIfFailed(_commandQueue->Signal(_fence.Get(), FenceValue));

    for (auto* list : commandLists)
    {
        list->FenceValue = FenceValue;
    }
}

const ComPtr<ID3D12Fence>& GDX12CommandQueue::GetFence()
{
    return _fence;
}

ComPtr<ID3D12Fence>& GDX12CommandQueue::GetOtherFence()
{
    return _otherFence;
}

void GDX12CommandQueue::CPUWaitForFenceValue(const uint64_t fenceValue)
{
    if (_fence->GetCompletedValue() >= fenceValue) { return; }

    HANDLE event = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
    _fence->SetEventOnCompletion(fenceValue, event);
    WaitForSingleObjectEx(event, INFINITE, FALSE);
    CloseHandle(event);
}

void GDX12CommandQueue::WaitForOtherFence(const uint64_t otherFenceValue)
{
    _commandQueue->Wait(_otherFence.Get(), otherFenceValue);
}

void GDX12CommandQueue::WaitForQueue(GDX12CommandQueue& producer, const uint64_t fenceValue)
{
    ThrowIfFailed(_commandQueue->Wait(producer.GetFence().Get(), fenceValue));
}

void GDX12CommandQueue::Flush()
{
    FenceValue++;
    ThrowIfFailed(_commandQueue->Signal(_fence.Get(), FenceValue));
    // Include native queue work submitted by presentation SDKs after our command lists.
    CPUWaitForFenceValue(FenceValue);
}

void GDX12CommandQueue::ClearCompletedLists()
{
    auto it = _workingCommandLists.begin();
    while (it != _workingCommandLists.end())
    {
        if ((*it)->FenceValue != 0 && _fence->GetCompletedValue() >= (*it)->FenceValue)
        {
            _availableCommandLists.push_back(std::move(*it));
            it = _workingCommandLists.erase(it);
        }
        else { ++it; }
    }
}

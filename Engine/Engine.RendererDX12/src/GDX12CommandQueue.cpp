#include "Engine.RendererDX12/GDX12CommandQueue.h"

#include "Engine.RendererDX12/GDX12CommandList.h"

GDX12CommandQueue::GDX12CommandQueue(GDX12Device* device)
    : FenceValue(0),  _device(device),  _lastDispatchedFenceValue(0)
{
    D3D12_COMMAND_QUEUE_DESC desc = {};
    desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    desc.NodeMask = 0;

    ThrowIfFailed(device->GetDevice()->CreateCommandQueue(&desc, IID_PPV_ARGS(&_commandQueue)));
    ThrowIfFailed(device->GetDevice()->CreateFence(FenceValue, 
        D3D12_FENCE_FLAG_SHARED | D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER, IID_PPV_ARGS(&_fence)));
}

void GDX12CommandQueue::Reset()
{
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

    auto commandList = std::make_unique<GDX12CommandList>(_device);
    rawPtr = commandList.get();
    _workingCommandLists.push_back(std::move(commandList));
    return rawPtr;
}

void GDX12CommandQueue::ExecuteCommandList(GDX12CommandList* commandList)
{
    commandList->GetCommandList()->Close();
    ID3D12CommandList* ppLists[] = { commandList->GetCommandList().Get() };
    _commandQueue->ExecuteCommandLists(1, ppLists);
    FenceValue++;
    _commandQueue->Signal(_fence.Get(), FenceValue);
    commandList->FenceValue = FenceValue;
    _lastDispatchedFenceValue = FenceValue;
}

void GDX12CommandQueue::ExecuteCommandLists(std::vector<GDX12CommandList*>& commandLists)
{
    std::vector<ID3D12CommandList*> ppLists;
    UINT commandListCount = commandLists.size();
    ppLists.reserve(commandListCount);

    for (UINT i = 0; i < commandListCount; ++i)
    {
        commandLists[i]->GetCommandList()->Close();
        ppLists.push_back(commandLists[i]->GetCommandList().Get());
    }

    _commandQueue->ExecuteCommandLists(commandListCount, ppLists.data());

    FenceValue++;
    _commandQueue->Signal(_fence.Get(), FenceValue);

    for (UINT i = 0; i < commandListCount; ++i) { commandLists[i]->FenceValue = FenceValue; }
    _lastDispatchedFenceValue = FenceValue;
}

const ComPtr<ID3D12Fence>& GDX12CommandQueue::GetFence()
{
    return _fence;
}

ComPtr<ID3D12Fence>& GDX12CommandQueue::GetOtherFence()
{
    return _otherFence;
}

void GDX12CommandQueue::CPUWaitForFenceValue(uint64_t fenceValue)
{
    if (_fence->GetCompletedValue() >= fenceValue) { return; }

    HANDLE event = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
    _fence->SetEventOnCompletion(fenceValue, event);
    WaitForSingleObjectEx(event, INFINITE, FALSE);
    CloseHandle(event);
}

void GDX12CommandQueue::WaitForOtherFence(uint64_t otherFenceValue)
{
    _commandQueue->Wait(_otherFence.Get(), otherFenceValue);
}

void GDX12CommandQueue::Flush()
{
    FenceValue++;
    _commandQueue->Signal(_fence.Get(), FenceValue);
    if (_lastDispatchedFenceValue > 0) { CPUWaitForFenceValue(_lastDispatchedFenceValue); }
}

void GDX12CommandQueue::ClearCompletedLists()
{
    auto it = _workingCommandLists.begin();
    while (it != _workingCommandLists.end())
    {
        if (_fence->GetCompletedValue() >= (*it)->FenceValue)
        {
            _availableCommandLists.push_back(std::move(*it));
            it = _workingCommandLists.erase(it);
        }
        else { ++it; }
    }
}

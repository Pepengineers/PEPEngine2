#include "Engine.RendererDX12/GDX12CommandQueue.h"

#include "Engine.RendererDX12/GDX12CommandList.h"

GDX12CommandQueue::GDX12CommandQueue(GDX12Device* device)
    : FenceValue(0), _device(device)
{
    D3D12_COMMAND_QUEUE_DESC desc = {};
    desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    desc.NodeMask = 0;

    ThrowIfFailed(device->GetDevice()->CreateCommandQueue(&desc, IID_PPV_ARGS(&_commandQueue)));
    ThrowIfFailed(device->GetDevice()->CreateFence(FenceValue, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&_fence)));

    _commandListExecutorThread = std::thread(&GDX12CommandQueue::ProcessInFlightCommandLists, this);
}

void GDX12CommandQueue::Reset()
{
    _commandQueue.Reset();
    _fence.Reset();

    std::shared_ptr<GDX12CommandList> list;
    while (_availableCommandLists.TryPop(list)) {}
    while (_workingCommandLists.TryPop(list)) {}
}

GDX12CommandQueue::~GDX12CommandQueue()
{
    _isExecutorAlive = false;
    _executorCondition.notify_one();
    _commandListExecutorThread.join();
    Reset();
}

ComPtr<ID3D12CommandQueue> GDX12CommandQueue::GetCommandQueue()
{
    return _commandQueue;
}

std::shared_ptr<GDX12CommandList>& GDX12CommandQueue::GetCommandList()
{
    std::shared_ptr<GDX12CommandList> commandList;

    if (_availableCommandLists.TryPop(commandList))
    {
        commandList->Reset();
        _workingCommandLists.Push(commandList);
        return commandList;
    }

    commandList = std::make_shared<GDX12CommandList>(_device);
    _workingCommandLists.Push(commandList);
    return commandList;
}

void GDX12CommandQueue::ExecuteCommandList(std::shared_ptr<GDX12CommandList> commandList)
{
    commandList->GetCommandList()->Close();
    ID3D12CommandList* ppLists[] = { commandList->GetCommandList().Get() };
    _commandQueue->ExecuteCommandLists(1, ppLists);
    FenceValue++;
    _commandQueue->Signal(_fence.Get(), FenceValue);
    commandList->FenceValue = FenceValue;
    _executorCondition.notify_one();
}

ComPtr<ID3D12Fence> GDX12CommandQueue::GetFence()
{
    return _fence;
}

void GDX12CommandQueue::WaitForFenceValue(uint64_t fenceValue)
{
    if (_fence->GetCompletedValue() >= fenceValue) { return; }

    HANDLE event = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);

    HRESULT hr = _fence->SetEventOnCompletion(fenceValue, event);
    WaitForSingleObjectEx(event, INFINITE, FALSE);
    CloseHandle(event);
}

void GDX12CommandQueue::ProcessInFlightCommandLists()
{
    while (_isExecutorAlive)
    {
        std::unique_lock<std::mutex> lock(_executorMutex);
        _executorCondition.wait(lock, [this] { return !_workingCommandLists.Empty() || !_isExecutorAlive; });

        std::shared_ptr<GDX12CommandList> commandList;

        if (_workingCommandLists.TryPop(commandList))
        {
            if (_fence->GetCompletedValue() >= commandList->FenceValue)
            {
                commandList->Reset();
                _availableCommandLists.Push(commandList);
            }
            else
            {
                WaitForFenceValue(commandList->FenceValue);
                commandList->Reset();
                _availableCommandLists.Push(commandList);
            }
        }
    }
}

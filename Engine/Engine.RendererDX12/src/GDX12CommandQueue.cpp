#include "Engine.RendererDX12/GDX12CommandQueue.h"

GDX12CommandQueue::GDX12CommandQueue(GDX12Device* device) 
    : FenceValue(0)
{
    D3D12_COMMAND_QUEUE_DESC desc = {};
    desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    desc.NodeMask = 0;

    ThrowIfFailed(device->GetDevice()->CreateCommandQueue(&desc, IID_PPV_ARGS(&_commandQueue)));
    ThrowIfFailed(device->GetDevice()->CreateFence(FenceValue, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&_fence)));
}

void GDX12CommandQueue::Reset()
{
    _commandQueue.Reset();
    _fence.Reset();
}

GDX12CommandQueue::~GDX12CommandQueue()
{
    Reset();
}

ComPtr<ID3D12CommandQueue> GDX12CommandQueue::GetCommandQueue()
{
    return _commandQueue;
}

ComPtr<ID3D12Fence> GDX12CommandQueue::GetFence()
{
    return _fence;
}

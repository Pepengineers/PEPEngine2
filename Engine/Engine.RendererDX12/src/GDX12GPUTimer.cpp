#include "Engine.RendererDX12/GDX12GPUTimer.h"

#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12CommandQueue.h"
#include "Engine.RendererDX12/GDX12Device.h"

GDX12GPUTimer::GDX12GPUTimer()
    : _commandQueue(nullptr)
    , _timestampFrequency(0)
    , _timeMS(-1.0)
    , _activeFrameIndex(0)
{
}

GDX12GPUTimer::GDX12GPUTimer(GDX12Device* device)
    : GDX12GPUTimer()
{
    Initialize(device);
}

GDX12GPUTimer::~GDX12GPUTimer()
{
    Reset();
}

void GDX12GPUTimer::Initialize(GDX12Device* device)
{
    Reset();

    if (!device) { return; }

    _commandQueue = device->GetCommandQueue();
    ThrowIfFailed(_commandQueue->GetCommandQueue()->GetTimestampFrequency(&_timestampFrequency));

    for (auto& frame : _frames)
    {
        D3D12_QUERY_HEAP_DESC queryHeapDesc = {};
        queryHeapDesc.Count = QueryCount;
        queryHeapDesc.NodeMask = 0;
        queryHeapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;

        ThrowIfFailed(device->GetDevice()->CreateQueryHeap(&queryHeapDesc, IID_PPV_ARGS(&frame.QueryHeap)));

        CD3DX12_HEAP_PROPERTIES readbackHeapProperties(D3D12_HEAP_TYPE_READBACK);
        CD3DX12_RESOURCE_DESC readbackBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(UINT64) * QueryCount);

        ThrowIfFailed(device->GetDevice()->CreateCommittedResource(
            &readbackHeapProperties,
            D3D12_HEAP_FLAG_NONE,
            &readbackBufferDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&frame.ReadbackBuffer)));

        ThrowIfFailed(frame.ReadbackBuffer->Map(0, nullptr, reinterpret_cast<void**>(&frame.MappedReadbackData)));
    }
}

void GDX12GPUTimer::Reset()
{
    for (auto& frame : _frames)
    {
        if (frame.ReadbackBuffer && frame.MappedReadbackData)
        {
            frame.ReadbackBuffer->Unmap(0, nullptr);
        }

        frame.QueryHeap.Reset();
        frame.ReadbackBuffer.Reset();
        frame.MappedReadbackData = nullptr;
        frame.FenceValue = 0;
    }

    _commandQueue = nullptr;
    _timestampFrequency = 0;
    _timeMS = -1.0;
    _activeFrameIndex = 0;
}

void GDX12GPUTimer::Start(GDX12CommandList* commandList)
{
    if (!commandList) { return; }

    UpdateResolvedFrames();

    auto& frame = _frames[_activeFrameIndex];
    if (!frame.QueryHeap) { return; }

    if (frame.FenceValue != 0 && _commandQueue && _commandQueue->GetFence()->GetCompletedValue() < frame.FenceValue)
    {
        _commandQueue->CPUWaitForFenceValue(frame.FenceValue);
        UpdateResolvedFrames();
    }

    commandList->GetCommandList()->EndQuery(frame.QueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, StartQueryIndex);
}

void GDX12GPUTimer::Stop(GDX12CommandList* commandList)
{
    if (!commandList) { return; }

    auto& frame = _frames[_activeFrameIndex];
    if (!frame.QueryHeap || !frame.ReadbackBuffer) { return; }

    commandList->GetCommandList()->EndQuery(frame.QueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, EndQueryIndex);
    commandList->GetCommandList()->ResolveQueryData(
        frame.QueryHeap.Get(),
        D3D12_QUERY_TYPE_TIMESTAMP,
        0,
        QueryCount,
        frame.ReadbackBuffer.Get(),
        0);
}

void GDX12GPUTimer::OnCommandListExecuted(UINT64 fenceValue)
{
    _frames[_activeFrameIndex].FenceValue = fenceValue;
    _activeFrameIndex = (_activeFrameIndex + 1) % BufferedFrameCount;
}

double GDX12GPUTimer::GetTimeMS()
{
    UpdateResolvedFrames();
    return _timeMS;
}

void GDX12GPUTimer::UpdateResolvedFrames()
{
    if (!_commandQueue || _timestampFrequency == 0) { return; }

    const UINT64 completedFenceValue = _commandQueue->GetFence()->GetCompletedValue();
    UINT64 latestResolvedFenceValue = 0;
    double latestResolvedTimeMS = _timeMS;

    for (auto& frame : _frames)
    {
        if (frame.FenceValue == 0 || !frame.MappedReadbackData || completedFenceValue < frame.FenceValue)
        {
            continue;
        }

        const UINT64 startTimestamp = frame.MappedReadbackData[StartQueryIndex];
        const UINT64 endTimestamp = frame.MappedReadbackData[EndQueryIndex];
        const UINT64 elapsedTicks = endTimestamp > startTimestamp ? endTimestamp - startTimestamp : 0;
        const double resolvedTimeMS = static_cast<double>(elapsedTicks) * 1000.0 / static_cast<double>(_timestampFrequency);

        if (frame.FenceValue >= latestResolvedFenceValue)
        {
            latestResolvedFenceValue = frame.FenceValue;
            latestResolvedTimeMS = resolvedTimeMS;
        }

        frame.FenceValue = 0;
    }

    if (latestResolvedFenceValue != 0)
    {
        _timeMS = latestResolvedTimeMS;
    }
}

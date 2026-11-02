#include "Engine.RendererDX12/GDX12GPUTimer.h"

#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12CommandQueue.h"
#include "Engine.RendererDX12/GDX12Device.h"

GDX12GPUTimer::GDX12GPUTimer()
    : _commandQueue(nullptr)
    , _mappedReadbackData(nullptr)
    , _timestampFrequency(0)
    , _timeMS(-1.0)
    , _activeFrameIndex(0)
    , _frameFenceValues{}
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

    D3D12_QUERY_HEAP_DESC queryHeapDesc = {};
    queryHeapDesc.Count = BufferedFrameCount * QueryCount;
    queryHeapDesc.NodeMask = 0;
    queryHeapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;

    ThrowIfFailed(device->GetDevice()->CreateQueryHeap(&queryHeapDesc, IID_PPV_ARGS(&_queryHeap)));

    CD3DX12_HEAP_PROPERTIES readbackHeapProperties(D3D12_HEAP_TYPE_READBACK);
    CD3DX12_RESOURCE_DESC readbackBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(UINT64) * BufferedFrameCount * QueryCount);

    ThrowIfFailed(device->GetDevice()->CreateCommittedResource(
        &readbackHeapProperties, D3D12_HEAP_FLAG_NONE,
        &readbackBufferDesc, D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_PPV_ARGS(&_readbackBuffer)));

    ThrowIfFailed(_readbackBuffer->Map(0, nullptr, reinterpret_cast<void**>(&_mappedReadbackData)));
}

void GDX12GPUTimer::Reset()
{
    if (_readbackBuffer && _mappedReadbackData) { _readbackBuffer->Unmap(0, nullptr); }

    _commandQueue = nullptr;
    _queryHeap.Reset();
    _readbackBuffer.Reset();
    _mappedReadbackData = nullptr;
    _timestampFrequency = 0;
    _timeMS = -1.0;
    _activeFrameIndex = 0;
    _frameFenceValues.fill(0);
}

void GDX12GPUTimer::Start(GDX12CommandList* commandList)
{
    if (!commandList) { return; }

    UpdateResolvedFrames();

    if (!_queryHeap) { return; }

    if (_frameFenceValues[_activeFrameIndex] != 0 && _commandQueue && _commandQueue->GetFence()->GetCompletedValue() < _frameFenceValues[_activeFrameIndex])
    {
        _commandQueue->CPUWaitForFenceValue(_frameFenceValues[_activeFrameIndex]);
        UpdateResolvedFrames();
    }

    commandList->GetCommandList()->EndQuery(_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, GetStartQueryIndex(_activeFrameIndex));
}

void GDX12GPUTimer::Stop(GDX12CommandList* commandList)
{
    if (!commandList) { return; }

    if (!_queryHeap || !_readbackBuffer) { return; }

    commandList->GetCommandList()->EndQuery(_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, GetEndQueryIndex(_activeFrameIndex));
    commandList->GetCommandList()->ResolveQueryData(
        _queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
        GetStartQueryIndex(_activeFrameIndex), QueryCount,
        _readbackBuffer.Get(), GetReadbackOffset(_activeFrameIndex));
}

void GDX12GPUTimer::OnCommandListExecuted(UINT64 fenceValue)
{
    _frameFenceValues[_activeFrameIndex] = fenceValue;
    _activeFrameIndex = (_activeFrameIndex + 1) % BufferedFrameCount;
}

double GDX12GPUTimer::GetTimeMS()
{
    UpdateResolvedFrames();
    return _timeMS;
}

void GDX12GPUTimer::UpdateResolvedFrames()
{
    if (!_commandQueue || !_mappedReadbackData || _timestampFrequency == 0) { return; }

    const UINT64 completedFenceValue = _commandQueue->GetFence()->GetCompletedValue();
    UINT64 latestResolvedFenceValue = 0;
    double latestResolvedTimeMS = _timeMS;

    for (UINT frameIndex = 0; frameIndex < BufferedFrameCount; ++frameIndex)
    {
        const UINT64 fenceValue = _frameFenceValues[frameIndex];
        if (fenceValue == 0 || completedFenceValue < fenceValue) { continue; }

        const UINT startQueryIndex = GetStartQueryIndex(frameIndex);
        const UINT endQueryIndex = GetEndQueryIndex(frameIndex);
        const UINT64 startTimestamp = _mappedReadbackData[startQueryIndex];
        const UINT64 endTimestamp = _mappedReadbackData[endQueryIndex];
        const UINT64 elapsedTicks = endTimestamp > startTimestamp ? endTimestamp - startTimestamp : 0;
        const double resolvedTimeMS = static_cast<double>(elapsedTicks) * 1000.0 / static_cast<double>(_timestampFrequency);

        if (fenceValue >= latestResolvedFenceValue)
        {
            latestResolvedFenceValue = fenceValue;
            latestResolvedTimeMS = resolvedTimeMS;
        }

        _frameFenceValues[frameIndex] = 0;
    }

    if (latestResolvedFenceValue != 0) { _timeMS = latestResolvedTimeMS; }
}

UINT GDX12GPUTimer::GetStartQueryIndex(UINT frameIndex) const
{
    return frameIndex * QueryCount;
}

UINT GDX12GPUTimer::GetEndQueryIndex(UINT frameIndex) const
{
    return GetStartQueryIndex(frameIndex) + 1;
}

UINT64 GDX12GPUTimer::GetReadbackOffset(UINT frameIndex) const
{
    return static_cast<UINT64>(GetStartQueryIndex(frameIndex)) * sizeof(UINT64);
}

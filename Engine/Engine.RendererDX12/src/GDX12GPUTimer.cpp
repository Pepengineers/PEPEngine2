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
    Initialize(device, device ? device->GetCommandQueue() : nullptr);
}

void GDX12GPUTimer::Initialize(GDX12Device* device, GDX12CommandQueue* queue, UINT sampleCount)
{
    Reset();

    if (!device || !queue) { return; }

    const bool copyQueue = queue->GetType() == D3D12_COMMAND_LIST_TYPE_COPY;
    if (copyQueue)
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS3 options{};
        if (FAILED(device->GetDevice()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &options, sizeof(options))) ||
            !options.CopyQueueTimestampQueriesSupported)
        { return; }
    }

    _commandQueue = queue;
    ThrowIfFailed(_commandQueue->GetCommandQueue()->GetTimestampFrequency(&_timestampFrequency));
    _samples.resize(sampleCount);

    D3D12_QUERY_HEAP_DESC queryHeapDesc = {};
    queryHeapDesc.Count = sampleCount * QueryCount;
    queryHeapDesc.NodeMask = 0;
    queryHeapDesc.Type = copyQueue ? D3D12_QUERY_HEAP_TYPE_COPY_QUEUE_TIMESTAMP : D3D12_QUERY_HEAP_TYPE_TIMESTAMP;

    ThrowIfFailed(device->GetDevice()->CreateQueryHeap(&queryHeapDesc, IID_PPV_ARGS(&_queryHeap)));

    CD3DX12_HEAP_PROPERTIES readbackHeapProperties(D3D12_HEAP_TYPE_READBACK);
    CD3DX12_RESOURCE_DESC readbackBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(UINT64) * sampleCount * QueryCount);

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
    _samples.clear();
}

bool GDX12GPUTimer::Start(GDX12CommandList* commandList, bool waitForSlot)
{
    if (!commandList || !_queryHeap) { return false; }

    UpdateResolvedFrames();

    const auto& sample = _samples[_activeFrameIndex];
    if (sample.FenceValue && sample.TimeMS < 0.0)
    {
        if (!waitForSlot) { return false; }
        _commandQueue->CPUWaitForFenceValue(sample.FenceValue);
        UpdateResolvedFrames();
    }

    RecordBegin(commandList, _activeFrameIndex);
    return true;
}

void GDX12GPUTimer::Stop(GDX12CommandList* commandList)
{
    if (!commandList) { return; }
    RecordEndAndResolve(commandList, _activeFrameIndex);
}

void GDX12GPUTimer::RecordBegin(GDX12CommandList* commandList, UINT sampleIndex)
{
    if (!_queryHeap) { return; }
    commandList->GetCommandList()->EndQuery(_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, GetStartQueryIndex(sampleIndex));
}

void GDX12GPUTimer::RecordEndAndResolve(GDX12CommandList* commandList, UINT sampleIndex)
{
    if (!_queryHeap || !_readbackBuffer) { return; }
    commandList->GetCommandList()->EndQuery(_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, GetEndQueryIndex(sampleIndex));
    commandList->GetCommandList()->ResolveQueryData(
        _queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
        GetStartQueryIndex(sampleIndex), QueryCount,
        _readbackBuffer.Get(), GetReadbackOffset(sampleIndex));
}

GDX12GPUTimer::TimestampRange GDX12GPUTimer::ReadResolvedTimestamps(UINT sampleIndex) const
{
    return {_mappedReadbackData[GetStartQueryIndex(sampleIndex)], _mappedReadbackData[GetEndQueryIndex(sampleIndex)]};
}

void GDX12GPUTimer::OnCommandListExecuted(UINT64 fenceValue)
{
    _samples[_activeFrameIndex] = { fenceValue, -1.0 };
    _activeFrameIndex = (_activeFrameIndex + 1) % static_cast<UINT>(_samples.size());
}

double GDX12GPUTimer::GetTimeMS()
{
    UpdateResolvedFrames();
    return _timeMS;
}

double GDX12GPUTimer::GetTimeMS(UINT64 fenceValue)
{
    if (!fenceValue) { return -1.0; }
    UpdateResolvedFrames();
    for (const auto& sample : _samples)
    { if (sample.FenceValue == fenceValue) { return sample.TimeMS; } }
    return -1.0;
}

void GDX12GPUTimer::UpdateResolvedFrames()
{
    if (!_commandQueue || !_mappedReadbackData || _timestampFrequency == 0) { return; }

    const UINT64 completedFenceValue = _commandQueue->GetFence()->GetCompletedValue();
    UINT64 latestResolvedFenceValue = 0;
    double latestResolvedTimeMS = _timeMS;

    for (UINT frameIndex = 0; frameIndex < _samples.size(); ++frameIndex)
    {
        auto& sample = _samples[frameIndex];
        const UINT64 fenceValue = sample.FenceValue;
        if (fenceValue == 0 || sample.TimeMS >= 0.0 || completedFenceValue < fenceValue) { continue; }

        const auto timestamps = ReadResolvedTimestamps(frameIndex);
        const UINT64 elapsedTicks = timestamps.End > timestamps.Begin ? timestamps.End - timestamps.Begin : 0;
        const double resolvedTimeMS = static_cast<double>(elapsedTicks) * 1000.0 / static_cast<double>(_timestampFrequency);
        sample.TimeMS = resolvedTimeMS;

        if (fenceValue >= latestResolvedFenceValue)
        {
            latestResolvedFenceValue = fenceValue;
            latestResolvedTimeMS = resolvedTimeMS;
        }

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

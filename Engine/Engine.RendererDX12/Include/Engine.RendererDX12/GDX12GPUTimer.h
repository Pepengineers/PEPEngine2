#pragma once

#include "Engine.RendererDX12/D3DHelpers.h"

class GDX12Device;
class GDX12CommandList;
class GDX12CommandQueue;

class GDX12GPUTimer
{
    static constexpr UINT BufferedFrameCount = 3;

public:
    struct TimestampRange
    {
        UINT64 Begin;
        UINT64 End;
    };

    GDX12GPUTimer();
    explicit GDX12GPUTimer(GDX12Device* device);
    ~GDX12GPUTimer();

    void Initialize(GDX12Device* device);
    void Initialize(GDX12Device* device, GDX12CommandQueue* queue, UINT sampleCount = BufferedFrameCount);
    void Reset();

    bool Start(GDX12CommandList* commandList, bool waitForSlot = true);
    void Stop(GDX12CommandList* commandList);
    void OnCommandListExecuted(UINT64 fenceValue);

    bool IsAvailable() const { return _queryHeap.Get() != nullptr; }
    double GetTimeMS();
    double GetTimeMS(UINT64 fenceValue);

    // For callers that own the sample slot and wait for its submissions before reading it.
    void RecordBegin(GDX12CommandList* commandList, UINT sampleIndex);
    void RecordEndAndResolve(GDX12CommandList* commandList, UINT sampleIndex);
    TimestampRange ReadResolvedTimestamps(UINT sampleIndex) const;
    UINT64 GetTimestampFrequency() const { return _timestampFrequency; }

private:
    void UpdateResolvedFrames();
    UINT GetStartQueryIndex(UINT frameIndex) const;
    UINT GetEndQueryIndex(UINT frameIndex) const;
    UINT64 GetReadbackOffset(UINT frameIndex) const;

    static constexpr UINT QueryCount = 2;

    struct Sample
    {
        UINT64 FenceValue = 0;
        double TimeMS = -1.0;
    };

    GDX12CommandQueue* _commandQueue;
    ComPtr<ID3D12QueryHeap> _queryHeap;
    ComPtr<ID3D12Resource> _readbackBuffer;
    UINT64* _mappedReadbackData;
    UINT64 _timestampFrequency;
    double _timeMS;
    UINT _activeFrameIndex;
    std::vector<Sample> _samples;
};

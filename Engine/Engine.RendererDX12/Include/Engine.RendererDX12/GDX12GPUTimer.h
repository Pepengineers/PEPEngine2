#pragma once

#include "Engine.RendererDX12/GDX12ResourceHandle.h"

#include "Engine.RendererDX12/D3DHelpers.h"

class GDX12Device;
class GDX12CommandList;
class GDX12CommandQueue;

struct GDX12GPUTimingTotals
{
    UINT64 Count = 0;
    double Milliseconds = 0;
};

class GDX12GPUTimer
{
public:
    GDX12GPUTimer();
    explicit GDX12GPUTimer(GDX12Device* device, GDX12CommandQueue* queue = nullptr);
    ~GDX12GPUTimer();

    void Initialize(GDX12Device* device, GDX12CommandQueue* queue = nullptr);
    void Reset();

    void Start(GDX12CommandList* commandList);
    void Stop(GDX12CommandList* commandList);
    void OnCommandListExecuted(UINT64 fenceValue);

    double GetTimeMS();
    GDX12GPUTimingTotals GetSampleTotals();

private:
    void UpdateResolvedFrames();
    UINT GetStartQueryIndex(UINT frameIndex) const;
    UINT GetEndQueryIndex(UINT frameIndex) const;
    UINT64 GetReadbackOffset(UINT frameIndex) const;

    static constexpr UINT BufferedFrameCount = 3;
    static constexpr UINT QueryCount = 2;

    GDX12CommandQueue* _commandQueue;
    ComPtr<ID3D12QueryHeap> _queryHeap;
    GDX12ResourceHandle _readbackBuffer;
    UINT64* _mappedReadbackData;
    UINT64 _timestampFrequency;
    double _timeMS;
    GDX12GPUTimingTotals _sampleTotals;
    UINT _activeFrameIndex;
    std::array<UINT64, BufferedFrameCount> _frameFenceValues;
};

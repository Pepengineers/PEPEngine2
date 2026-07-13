#pragma once

#include "Engine.RendererDX12/D3DHelpers.h"

class GDX12Device;
class GDX12CommandList;
class GDX12CommandQueue;

class GDX12GPUTimer
{
public:
    GDX12GPUTimer();
    explicit GDX12GPUTimer(GDX12Device* device);
    ~GDX12GPUTimer();

    void Initialize(GDX12Device* device);
    void Reset();

    void Start(GDX12CommandList* commandList);
    void Stop(GDX12CommandList* commandList);
    void OnCommandListExecuted(UINT64 fenceValue);

    double GetTimeMS();

private:
    struct GPUTimerFrame
    {
        ComPtr<ID3D12QueryHeap> QueryHeap;
        ComPtr<ID3D12Resource> ReadbackBuffer;
        UINT64* MappedReadbackData = nullptr;
        UINT64 FenceValue = 0;
    };

    void UpdateResolvedFrames();

    static constexpr UINT BufferedFrameCount = 3;
    static constexpr UINT QueryCount = 2;
    static constexpr UINT StartQueryIndex = 0;
    static constexpr UINT EndQueryIndex = 1;

    GDX12CommandQueue* _commandQueue;
    UINT64 _timestampFrequency;
    double _timeMS;
    UINT _activeFrameIndex;
    std::array<GPUTimerFrame, BufferedFrameCount> _frames;
};

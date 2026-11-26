#pragma once

#include "Engine.RendererDX12/FrameGraph/FrameGraph.h"
#include "Engine.RendererDX12/FrameGraph/FrameGraphResources.h"
#include "Engine.RendererDX12/GDX12GPUTimer.h"

// GPU timestamp queries and reports for one compiled graph.
struct FrameGraphDiagnostics
{
    struct CopySample
    {
        GDX12GPUTimer* Timer;
        EDeviceRole Device;
        UINT64 FenceValue;
        bool Exporting;
        uint64_t ResourceBytes;
    };

    struct CopyTiming
    {
        double TimeMS = 0.0;
        size_t Copies = 0;
        size_t Measured = 0;

        void Add(double ms);
        double GetCompleteTimeMS() const { return Measured == Copies ? TimeMS : -1.0; }
    };

    struct QueueTimestampSample
    {
        GDX12CommandQueue* Queue = nullptr;
        UINT64 Frequency = 0;
        GDX12GPUTimer::TimestampRange Timestamps{};
    };

    struct CompletedFrameTiming
    {
        uint64_t FrameNumber;
        uint32_t PassTimingSlot;
        std::array<CopyTiming, 2> CopyTimes{};
        FrameGraphCrossGPUCopyStats CrossGPUCopies;
        // Each adapter can submit work to a DIRECT and a COPY queue.
        std::array<QueueTimestampSample, 4> QueueSamples{};
    };

    std::map<NodeId, std::unique_ptr<GDX12GPUTimer>> PassTimers;
    std::map<GDX12CommandQueue*, GDX12GPUTimer> QueueTimers;
    std::optional<CompletedFrameTiming> LastCompletedFrame;
    uint64_t SubmittedCopyBytes = 0;
    uint64_t SubmittedCopyCommands = 0;

    GDX12GPUTimer& GetTransferTimer(GDX12CommandQueue* queue, const PinAddress& source, bool exporting, uint32_t slots);
    void CompleteFrame(uint64_t frame, uint32_t slot, uint32_t passTimingSlot,
        const std::vector<CopySample>& copies, const std::vector<GDX12CommandQueue*>& queues);
    FrameGraphPerFrameMetrics GetLatestFrameMetrics(const FrameGraphPlan& plan, const FrameGraphDefinition& definition) const;
    std::string GetDebugReport(const FrameGraphPlan& plan, const FrameGraphDefinition& definition,
        const FrameGraphCompileData& compileData, bool usesSecondary, const std::vector<std::string>& fallbacks,
        const std::map<PinAddress, FrameGraphMaterialization>* allocations,
        const std::map<PinAddress, FrameGraphMaterialization>* persistentAllocations);

private:
    // Each key identifies the logical output and its export or import operation.
    std::map<GDX12CommandQueue*, std::map<std::pair<PinAddress, bool>, GDX12GPUTimer>> _transferTimers;
};

#pragma once

#include "Engine.RendererDX12/FrameGraph/FrameGraph.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

class RenderingMetrics
{
public:
    struct Sample
    {
        double FPS = 0.0;
        double GPUPrimaryFrameMS = -1.0;
        double GPUSecondaryFrameMS = -1.0;
        FrameGraphCrossGPUCopyStats CrossGPUCopies;
        std::vector<FrameGraphPassTiming> PassTimings;
    };

    void Initialize(GDX12Device* primary, GDX12Device* secondary);
    void RecordPresentedFrame(const CompiledFrameGraph& frameGraph);
    void ResetInterval();
    void SaveReport() const;

    // Read on the application thread. Sampling may invalidate references to individual entries.
    const std::vector<Sample>& GetHistory() const { return _history; }

private:
    struct AdapterInfo
    {
        EDeviceRole Role;
        std::string Name;
        unsigned PCIeGeneration;
    };

    std::string ToJson() const;

    using Clock = std::chrono::steady_clock;
    bool _initialized = false;
    std::optional<Clock::time_point> _intervalStart;
    std::uint64_t _presentCount = 0;
    std::vector<Sample> _history;
    std::string _cpuName;
    std::uint64_t _ramBytes = 0;
    std::string _motherboardName;
    std::vector<AdapterInfo> _adapters;
};

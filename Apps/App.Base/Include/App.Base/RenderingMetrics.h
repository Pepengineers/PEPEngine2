#pragma once

#include "Engine.Core/ECS/Event.h"
#include "Engine.RendererDX12/FrameGraph/FrameGraph.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
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

    void Initialize();
    void BeginRun(const char* pipeline, std::chrono::seconds duration, const std::filesystem::path& reportDirectory = {});
    void RecordPresentedFrame(const CompiledFrameGraph& frameGraph);
    void ResetInterval();
    Event<> OnRunCompleted;

    // Read on the application thread. Sampling may invalidate references to individual entries.
    const std::vector<Sample>& GetHistory() const { return _history; }
    const std::filesystem::path& GetLastReportPath() const { return _lastReportPath; }

private:
    std::filesystem::path SaveReport(const CompiledFrameGraph& frameGraph) const;
    std::string ToJson(const CompiledFrameGraph& frameGraph) const;

    using Clock = std::chrono::steady_clock;
    bool _collecting = false;
    std::string _pipeline;
    std::filesystem::path _reportDirectory;
    std::filesystem::path _lastReportPath;
    std::chrono::seconds _runDuration{};
    Clock::duration _activeRenderTime{};
    std::optional<Clock::time_point> _lastPresent;
    std::optional<Clock::time_point> _intervalStart;
    std::uint64_t _presentCount = 0;
    std::vector<Sample> _history;
    std::string _cpuName;
    std::uint64_t _ramBytes = 0;
    std::string _motherboardName;
};

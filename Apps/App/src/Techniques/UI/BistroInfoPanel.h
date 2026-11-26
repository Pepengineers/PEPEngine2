#pragma once

#include "BistroBenchmarkLaunch.h"
#include "BistroComparePanel.h"
#include <array>
#include <chrono>
#include <map>
#include <vector>
#include <optional>
#include <string>
#include <utility>

class BistroBenchmark;
class RenderModule;
struct ImFont;
namespace BistroRender { class QueueSubmission; }

class BistroInfoPanel
{
public:
    void InitializeFont();
    BistroRun::Launch SelectedLaunch() const { return _launch; }
#if defined(_DEBUG) || defined(DEBUG)
    const BistroRun::Map* TakeDebugMap() { return std::exchange(_requestedDebugMap, nullptr); }
#endif
    void DrawSettings(RenderModule&, BistroBenchmark&);
    void DrawMeasurement(const BistroBenchmark&) const;
    void RecordPresent(std::chrono::steady_clock::time_point now, BistroRender::QueueSubmission&, bool manualGpuTimings);
    void ShowSettings(bool openResults, const std::string& error = {});

private:
    enum class Tab { Setup, Results };
    void DrawLaunchControls(BistroBenchmark& benchmark);
    void DrawPresentChart(float height) const;
    void ApplyPreset(const std::filesystem::path& path);

    BistroRun::Launch _launch;
    bool _parametersOpen = false;
#if defined(_DEBUG) || defined(DEBUG)
    const BistroRun::Map* _requestedDebugMap = nullptr;
#endif
    BistroComparePanel _compare;
    ImFont* _fullFont = nullptr;
    ImFont* _headingFont = nullptr;
    Tab _tab = Tab::Setup;
    std::string _displayError;
    bool _initializedSize = false;
    struct GpuWork
    {
        std::string Adapter;
        std::vector<std::pair<double, double>> Intervals;
    };
    std::map<unsigned, GpuWork> _gpuWork;
    uint64_t _timingEpoch = 0;
    size_t _packetCursor = 0;
    std::array<double, 180> _presentBeginMs{};
    std::array<float, 180> _presentMs{};
    size_t _count = 0, _next = 0;
    float _hudMeanMs = 0.f, _hudP95Ms = 0.f;
    std::chrono::steady_clock::time_point _hudStatsDue{};
    std::optional<std::chrono::steady_clock::time_point> _lastPresent;
};

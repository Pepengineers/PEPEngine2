#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <ios>
#include <future>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

struct ImFont;

class BistroComparePanel
{
  public:
    void UpdateHistory(const std::filesystem::path& latestCompleted, bool force = false);
    void DrawHistory(const std::filesystem::path& latestCompleted, std::filesystem::path& pendingPreset, ImFont* headingFont);

  private:
    struct Window
    {
        double Begin = -1, End = -1;
        std::string Section;
    };
    struct FrameTiming
    {
        double Fps = -1, P95 = -1;
        // Reports store aggregate FPS as sample count divided by total present-interval time.
        double MeanMs() const { return Fps > 0 ? 1000. / Fps : -1.; }
    };
    struct Pass
    {
        std::filesystem::path Path;
        std::string Label, Mask;
        Window Range;
        std::streamoff InlineOffset = -1;
        FrameTiming Timing;
    };
    using Values = std::map<std::string, std::string>;
    struct PlacementResult
    {
        Values Fields, Techniques, GpuSettings;
        bool Confirmed() const;
        int RunIndex = -1, PassIndex = -1;
    };
    struct Winner : PlacementResult
    {
        bool Available() const;
        Values Settings;
        std::vector<std::string> Improvements;
        std::array<PlacementResult, 2> Placements;
    };
    struct Run
    {
        std::filesystem::path Path;
        std::filesystem::file_time_type Modified;
        std::string Label, Mode, Profile, MapId, RouteId, FinalStatus, SelectedImage;
        std::string MapName, DisplayMode, Output, Timestamp;
        std::optional<double> SummaryFps, MinimumFps;
        double Target() const { return Profile == "Performance" ? 60. : Profile == "Quality" ? 30. : 0.; }
        const char* TargetStatus(double fps, const Winner* winner) const;
        bool AfterRender = false;
        Values Settings, Before, TrialPasses, MaskBits, Adapters;
        std::array<Winner, 2> Winners;
        std::string FinalMask, StopReason;
        std::vector<Pass> Passes;
    };
    struct Selection
    {
        int RunIndex = -1, PassIndex = -1;
        std::string Mode, Error;
        Window Range;
        bool AfterRender = false;
        PlacementResult Observed;
        Values Settings;
        struct Sample
        {
            double RouteSeconds, IntervalMs, FrameMs;
            size_t Number;
            std::string Section;
        };
        std::vector<Sample> Samples;
        std::vector<size_t> Trace;
        FrameTiming Timing;
        double MaxMs = 0, MaxFps = 0;
        double ScopeMeanMs = -1, ScopeP95Ms = -1;
    };
    struct PlotLine
    {
        const Selection* Data = nullptr;
        std::string Label;
    };
    using PlotLines = std::array<PlotLine, 2>;
    PlotLines RunLines(const Winner* placement, const Selection& current, std::array<Selection, 2>& cache);
    FrameTiming SummaryTiming(const Winner* winner, const Selection& selection) const;
    std::string ReportBody(const Run& run, const Winner* placement, const Selection& selection,
        std::vector<std::filesystem::path>& measuredPasses);
    void DrawExportStatus(const Run& run, const Winner* placement, const Selection& selection, bool requested);
    std::future<std::filesystem::path> _exportJob;
    std::filesystem::path _exportRun, _exportPath;
    std::string _exportError;
    void Refresh(bool force);
    bool _upscaled = false;
    ImFont* _headingFont = nullptr;
    void Select(Selection &, int run, int pass);
    void DrawSelection(Selection &, size_t index);
    static void DrawPlot(const PlotLines& lines, bool fps, double target = 0);
    static void DrawRouteMetrics(const FrameTiming& timing, ImFont* headingFont);
    static void DrawComparedTimings(const FrameTiming& a, const FrameTiming& b, const char* left, const char* right, bool comparable);
    static void DrawPlacementComparison(const Winner& winner);
    static void DrawTechniques(const PlacementResult& before, const PlacementResult& after,
        const char* title, const char* left, const char* right);
    void DrawRun(int index, std::filesystem::path& pendingPreset);
    int DrawRunList(bool compact);
    void DrawComparison();
    Selection _lastPass;
    std::array<Selection, 2> _placementPasses;
    std::filesystem::path _historyPath, _imageRun;
    bool _showComparison = false, _runFps = false;
    bool _loadComparison = false;
    std::string _openError;
    std::vector<Run> _runs;
    std::array<std::vector<std::string>, 3> _filterOptions;
    std::array<std::string, 3> _filters;
    std::array<Selection, 2> _selected;
    bool _fps = false, _indexed = false;
    std::filesystem::path _latestCompleted;
    std::string _notice;
};

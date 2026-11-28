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
#include <imgui/imgui.h>

struct ImFont;

class BistroComparePanel
{
  public:
    void UpdateHistory(const std::filesystem::path& latestCompleted, bool force = false);
    void DrawHistory(const std::filesystem::path& latestCompleted, std::filesystem::path& pendingPreset, ImFont* headingFont);
    bool CanToggleHistory() const { return !_runs.empty() && !_showComparison; }
    void DrawHistoryToggle(float size);

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
    struct ReconstructionComparison
    {
        std::vector<std::string> Participants;
        std::string Mode, Status, Winner;
    };
    struct Run
    {
        std::filesystem::path Path;
        std::filesystem::file_time_type Modified;
        std::string Label, Mode, Profile, MapId, RouteId, FinalStatus, SelectedImage;
        std::string MapName, DisplayMode, Output, Timestamp;
        std::optional<double> SummaryFps, MinimumFps;
        std::optional<double> BaseBudget, AddedRoutes, Budget, Elapsed;
        struct PhaseBudget
        {
            std::optional<double> Base, Added, Total;
        };
        std::array<PhaseBudget, 4> Phases;
        std::map<unsigned, ReconstructionComparison> Reconstructions;
        std::map<std::pair<std::string, std::string>, ReconstructionComparison> ReconstructionDiscovery;
        std::map<std::string, Values> ReconstructionTrials;
        double Target() const { return Profile == "Performance" ? 60. : Profile == "Quality" ? 30. : 0.; }
        const char* TargetStatus(double fps, const Winner* winner) const;
        bool AfterRender = false;
        Values Settings, Before, TrialPasses, MaskBits, Adapters;
        std::array<Winner, 2> Winners;
        std::string FinalMask, StopReason, ManualReconstructionFallback;
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
        std::string Caption(bool fps) const;
    };
    using PlotLines = std::array<PlotLine, 2>;
    using PlotSamples = std::array<const Selection::Sample*, 2>;
    static bool ComparableFrames(const PlotLines& lines);
    static constexpr auto FrameDeltaNote = "Delta requires matching route, measured window and timing method.";
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
    struct PlotView
    {
        struct Axis
        {
            double Min = 0, Max = 1;
            double Span() const { return Max - Min; }
            void Set(double min, double max);
            void Zoom(double anchor, double factor);
        };
        Axis X, Y;
        ImVec2 DragStart, DragLast;
        int DragButton = -1;
        bool FitY = false, Pinned = false;
        std::optional<double> CursorTime;
        void Zoom(double x, double y, double factor);
        void ZoomTime(double factor);
    };
    PlotView _plotView;
    void DrawPlot(const PlotLines& lines, bool& fps, double target, bool comparable);
    static void DrawPlotLegend(const PlotLines& lines, bool fps, double target);
    static PlotSamples DrawFrameTraces(const PlotLines& lines, bool fps, double target, PlotView* view = nullptr, float reservedHeight = 0);
    static void DrawPlotInspector(const PlotLines& lines, const PlotSamples& samples, PlotView& view, bool fps, bool comparable, int columns);
    static void DrawRouteMetrics(const FrameTiming& timing, ImFont* headingFont);
    static void DrawComparedTimings(const FrameTiming& a, const FrameTiming& b, const char* left, const char* right, bool comparable);
    static std::array<std::string, 8> ReconstructionRow(const Run&, const ReconstructionComparison&, const std::string&);
    static constexpr std::array SearchPhaseLabels{"Single GPU / Native", "Single GPU / Upscaled",
        "Multi-GPU / Native", "Multi-GPU / Upscaled"};
    static constexpr std::array<size_t, 6> DiscoveryColumns{0, 1, 2, 3, 4, 7};
    static constexpr std::array<size_t, 8> ComparisonColumns{0, 1, 2, 3, 4, 5, 6, 7};
    static constexpr std::array ReconstructionColumns{"Trial", "Requested upscaler", "Effective upscaler", "Post device",
        "Internal resolution", "FPS", "P95 (ms)", "Result"};
    static void DrawPlacementComparison(const Winner& winner);
    static void DrawTechniques(const PlacementResult& before, const PlacementResult& after,
        const char* title, const char* left, const char* right);
    void DrawRun(int index, std::filesystem::path& pendingPreset);
    int DrawRunList(bool compact);
    void DrawComparison();
    Selection _lastPass;
    std::array<Selection, 2> _placementPasses;
    std::filesystem::path _historyPath, _imageRun;
    bool _showHistory = false, _showComparison = false, _runFps = false;
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

#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <ios>
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
    struct Pass
    {
        std::filesystem::path Path;
        std::string Label, Mask;
        Window Range;
        std::streamoff InlineOffset = -1;
    };
    using Values = std::map<std::string, std::string>;
    struct PlacementResult
    {
        Values Fields, Techniques, GpuSettings;
    };
    struct Winner : PlacementResult
    {
        Values Settings;
        std::vector<std::string> Improvements;
        std::array<PlacementResult, 2> Placements;
        int RunIndex = -1, PassIndex = -1;
    };
    struct Run
    {
        std::filesystem::path Path;
        std::filesystem::file_time_type Modified;
        std::string Label, Mode, Profile, MapId, RouteId, FinalStatus, SelectedImage;
        std::string MapName, DisplayMode, Output, Timestamp;
        std::optional<double> SummaryFps, MinimumFps;
        bool AfterRender = false;
        Values Settings, Before, TrialPasses, MaskBits;
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
        double Fps = 0, P95 = 0, MaxMs = 0, MaxFps = 0;
        double ScopeMeanMs = -1, ScopeP95Ms = -1;
    };
    void Refresh(bool force);
    bool _upscaled = false;
    ImFont* _headingFont = nullptr;
    void Select(Selection &, int run, int pass);
    void DrawSelection(Selection &, size_t index);
    static void DrawPlot(const Selection* series, size_t count, bool fps, double target = 0);
    static void DrawPlacementComparison(const Winner& winner);
    static void DrawTechniques(const PlacementResult& before, const PlacementResult& after,
        const char* title, const char* left, const char* right);
    void DrawRun(int index, std::filesystem::path& pendingPreset);
    int DrawRunList(bool compact);
    void DrawComparison();
    Selection _lastPass;
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

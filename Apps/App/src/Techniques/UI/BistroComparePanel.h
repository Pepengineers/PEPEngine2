#pragma once

#include <array>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

class BistroComparePanel
{
  public:
    void UpdateHistory(const std::filesystem::path& latestCompleted, bool force = false);
    bool LatestQuality(const char* mode) const;
    bool LatestSingleGpuIsAsync() const;
    void DrawLatest(const char* mode, const char* profile, bool fps);
    void Draw(const std::filesystem::path& latestCompleted);

  private:
    struct Pass
    {
        std::filesystem::path Path;
        std::string Label;
    };
    struct Run
    {
        std::filesystem::path Path;
        std::string Label, Mode, Profile;
        std::map<std::string, std::string> Settings;
        std::vector<Pass> Passes;
    };
    struct Selection
    {
        int RunIndex = -1, PassIndex = -1;
        std::string Mode, Error;
        std::map<std::string, std::string> Settings;
        struct Sample
        {
            double RouteSeconds, IntervalMs, FrameMs;
            size_t Number;
            std::string Section;
        };
        std::vector<Sample> Samples;
        std::vector<size_t> Trace;
        double Fps = 0, P95 = 0, MaxMs = 0, MaxFps = 0;
    };
    void Refresh();
    void Select(Selection &, int run, int pass);
    void DrawSelection(Selection &, const char *label);
    static void DrawPlot(const Selection* series, size_t count, bool fps);
    using Filter = std::pair<std::string, std::string>;
    struct Entry
    {
        std::filesystem::file_time_type Completed;
        int RunIndex, PassIndex;
    };
    std::filesystem::file_time_type LatestTime(const Filter& filter) const;
    std::map<Filter, std::vector<Entry>> _completed;
    std::map<Filter, Selection> _lastPass;
    bool _loadComparison = false;
    std::string _openError;
    std::vector<Run> _runs;
    std::array<Selection, 2> _selected;
    bool _fps = false, _indexed = false;
    std::filesystem::path _latestCompleted;
    std::string _notice;
};

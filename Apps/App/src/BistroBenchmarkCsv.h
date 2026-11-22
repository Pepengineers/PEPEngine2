#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace BistroBenchmarkData
{
struct Frame { double RouteSeconds, Milliseconds, IntervalMs; size_t Section; unsigned Groups = 0; };
struct Stats { size_t Count = 0; double Mean = 0, P95 = 0; };

inline bool CoversWindow(const std::vector<Frame>& frames, double begin, double end)
{
    if (frames.size() < 3) return false;
    // A presented frame must sample each second of the route, including the endpoints.
    constexpr double MaxGap = 1;
    for (const auto& frame : frames)
    {
        if (frame.RouteSeconds - begin > MaxGap) return false;
        begin = frame.RouteSeconds;
    }
    return end - begin <= MaxGap;
}

inline Stats Measure(const std::vector<Frame>& frames, double begin, double end)
{
    std::vector<double> samples;
    double sum = 0;
    for (const auto& frame : frames)
        if (begin <= frame.RouteSeconds && frame.RouteSeconds < end)
        {
            samples.push_back(frame.Milliseconds);
            sum += frame.Milliseconds;
        }
    if (samples.empty()) return {};
    const auto p95 = samples.begin() + (samples.size() * 95 - 1) / 100;
    std::nth_element(samples.begin(), p95, samples.end());
    return {samples.size(), sum / samples.size(), *p95};
}

class Csv
{
public:
    void Open(const std::filesystem::path& path)
    {
        _directory = path.parent_path() / path.stem();
        std::filesystem::create_directories(_directory);
        _pass = 0;
        _trialId.clear();
        _file.clear();
        _file.open(path);
        Check();
        _file << Header;
    }

    void Config(std::string_view name, std::string_view value)
    {
        auto row = Row("config");
        row[Name] = name;
        row[Value] = value;
        // Placement search names trial records Trial.<id>.<field>.
        if (name.substr(0, 6) == "Trial.")
        {
            const auto dot = name.find('.', 6);
            const auto id = name.substr(6, dot - 6);
            if (dot != name.npos && !id.empty() && id.find_first_not_of("0123456789") == id.npos)
            {
                if (id != _trialId) OpenTrial(id);
                Write(_trialRows, row);
                const auto field = name.substr(dot + 1);
                if (field == "FPS" || field == "P95ms" || field == "Status" ||
                    field == "Source" || field == "Scope" || field == "OutputResolution" ||
                    field == "RenderResolution")
                    Write(_file, row);
                if (field == "Status" || field == "EquivalentTrial" ||
                    field.find("RemovalEvidence") != field.npos) Flush();
                return;
            }
        }
        Write(_file, row);
    }

    template<class SectionName>
    void Leg(const char* stage, unsigned mask, const std::vector<Frame>& frames, double begin,
        double end, double elapsed, const char* status, SectionName sectionName, unsigned round, bool fullFlight)
    {
        const auto filename = "pass-" + std::to_string(++_pass) + ".csv";
        const auto path = _directory / filename;
        auto pending = path;
        pending += ".tmp";
        std::ofstream output(pending);
        output << Header;
        std::vector<double> intervals;
        intervals.reserve(frames.size());
        double measuredMs = 0;
        double total = 0, minFps = 0, minMspf = 0, maxFps = 0, maxMspf = 0;
        size_t count = 0;
        const char* section = "";
        double interval = begin;
        const auto intervalEnd = [&] { return std::floor(interval) + 1; };
        const auto emit = [&]
        {
            if (!count || total <= 0) return;
            const double mspf = total / count;
            const double fps = 1000 / mspf;
            minFps = minFps == 0 ? fps : std::min(minFps, fps);
            minMspf = minMspf == 0 ? mspf : std::min(minMspf, mspf);
            maxFps = std::max(maxFps, fps);
            maxMspf = std::max(maxMspf, mspf);
            auto row = Row("interval", stage, mask);
            if (round) row[Round] = std::to_string(round);
            row[Section] = section;
            row[Begin] = Number(interval);
            row[End] = Number(intervalEnd());
            row[Count] = std::to_string(count);
            row[Fps] = Number(fps);
            row[Mspf] = Number(mspf);
            row[MinFps] = Number(minFps);
            row[MinMspf] = Number(minMspf);
            row[MaxFps] = Number(maxFps);
            row[MaxMspf] = Number(maxMspf);
            Write(output, row);
        };
        for (size_t i = 0; i < frames.size(); ++i)
        {
            const auto& frame = frames[i];
            intervals.push_back(frame.IntervalMs);
            measuredMs += frame.IntervalMs;
            if (frame.RouteSeconds >= intervalEnd())
            {
                emit();
                total = 0;
                count = 0;
                interval = std::floor(frame.RouteSeconds);
            }
            ++count;
            if (count == 1) section = sectionName(frame.Section);
            total += frame.IntervalMs;
            auto row = Row("frame", stage, mask);
            if (round) row[Round] = std::to_string(round);
            row[Section] = sectionName(frame.Section);
            row[FrameIndex] = std::to_string(i + 1);
            row[Route] = Number(frame.RouteSeconds);
            row[Ms] = Number(frame.Milliseconds);
            row[Interval] = Number(frame.IntervalMs);
            Write(output, row);
        }
        if (end >= intervalEnd()) emit();
        const char* summarySection = frames.empty() ? "" : fullFlight ? "Full route" : sectionName(frames.front().Section);
        Summary(output, stage, mask, summarySection, begin, end,
            elapsed, Measure(frames, begin, end), status, round);
        auto entry = Row("pass", stage, mask);
        entry[Name] = "PassCSV";
        entry[Value] = (_directory.filename() / filename).generic_string();
        entry[Begin] = Number(begin);
        entry[End] = Number(end);
        entry[Wall] = Number(elapsed);
        entry[Status] = status;
        entry[Section] = summarySection;
        entry[Count] = std::to_string(frames.size());
        if (round) entry[Round] = std::to_string(round);
        if (!intervals.empty())
        {
            if (measuredMs > 0) entry[Fps] = Number(1000 * frames.size() / measuredMs);
            const auto rank = intervals.begin() + (intervals.size() * 95 - 1) / 100;
            std::nth_element(intervals.begin(), rank, intervals.end());
            entry[P95] = Number(*rank);
        }
        auto detail = entry;
        detail[Name] = "RunCSV";
        detail[Value] = "../" + _directory.filename().generic_string() + ".csv";
        Write(output, detail);
        if (!_trialId.empty())
        {
            auto trial = Row("config");
            trial[Name] = "TrialCSV";
            trial[Value] = "trial-" + _trialId + ".csv";
            Write(output, trial);
        }
        output.close();
        if (!output) throw std::runtime_error("Cannot save Bistro benchmark pass");
        FlushTrial();
        std::filesystem::rename(pending, path);
        Write(_file, entry);
        _file.flush();
        Check();
    }

    void Window(const char* stage, unsigned mask, const char* section, double begin, double end, Stats stats)
    {
        Summary(_file, stage, mask, section, begin, end, 0, stats, "measured");
    }

    void Result(unsigned mask, double seconds, const char* reason)
    {
        auto row = Row("result", "search", mask);
        row[Wall] = Number(seconds);
        row[Status] = reason;
        Write(_file, row);
    }

    void Decision(unsigned round, unsigned mask, std::string_view name, std::string_view value)
    {
        auto row = Row("decision", "Elimination", mask);
        row[Round] = std::to_string(round);
        row[Name] = name;
        row[Value] = value;
        Write(_file, row);
    }

    bool IsOpen() const { return _file.is_open(); }
    void Flush()
    {
        FlushTrial();
        _file.flush();
        Check();
    }

    void AppendResult(const std::filesystem::path& path, unsigned mask, double seconds, const char* reason)
    {
        _file.open(path, std::ios::app);
        Check();
        Result(mask, seconds, reason);
        Close();
    }

    void Close()
    {
        CloseTrial();
        if (!_file.is_open()) return;
        _file.close();
        Check();
    }

    static std::string Number(double value)
    {
        char buffer[64];
        const auto [end, error] = std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::fixed, 6);
        if (error != std::errc{}) throw std::runtime_error("Cannot format Bistro benchmark metric");
        return {buffer, end};
    }

private:
    static constexpr const char* Header =
        "type,stage,mask,section,frame,route_seconds,frame_ms,frame_interval_ms,"
        "window_start,window_end,frames,fps,mspf,min_fps,min_mspf,max_fps,max_mspf,"
        "mean_ms,p95_ms,measured_seconds,wall_seconds,status,name,value,round\n";
    enum Column { Type, Stage, Mask, Section, FrameIndex, Route, Ms, Interval, Begin, End, Count,
        Fps, Mspf, MinFps, MinMspf, MaxFps, MaxMspf, Mean, P95, Measured, Wall, Status, Name, Value, Round, Width };
    using Fields = std::array<std::string, Width>;

    static Fields Row(const char* type, const char* stage = "", unsigned mask = 0)
    {
        Fields row{};
        row[Type] = type;
        row[Stage] = stage;
        if (*stage) row[Mask] = std::to_string(mask);
        return row;
    }

    static void Write(std::ostream& output, const Fields& row)
    {
        for (size_t i = 0; i < row.size(); ++i)
        {
            if (i) output.put(',');
            const auto& field = row[i];
            if (field.find_first_of(",\"\r\n") == std::string::npos) output << field;
            else
            {
                output.put('"');
                for (char ch : field)
                {
                    if (ch == '"') output.put('"');
                    output.put(ch);
                }
                output.put('"');
            }
        }
        output.put('\n');
    }

    static void Summary(std::ostream& output, const char* stage, unsigned mask, const char* section,
        double begin, double end, double elapsed, Stats stats, const char* status, unsigned round = 0)
    {
        auto row = Row("summary", stage, mask);
        row[Section] = section;
        row[Begin] = Number(begin);
        row[End] = Number(end);
        row[Measured] = Number(end - begin);
        row[Wall] = Number(elapsed);
        row[Count] = std::to_string(stats.Count);
        if (stats.Count)
        {
            row[Mean] = Number(stats.Mean);
            row[P95] = Number(stats.P95);
        }
        row[Status] = status;
        if (round) row[Round] = std::to_string(round);
        Write(output, row);
    }

    void Check() const
    {
        if (!_file) throw std::runtime_error("Cannot write Bistro benchmark CSV");
    }

    void FlushTrial()
    {
        if (!_trialFile.is_open()) return;
        _trialFile << _trialRows.str();
        _trialRows.str({});
        _trialFile.flush();
        if (!_trialFile) throw std::runtime_error("Cannot write Bistro benchmark trial");
    }

    void CloseTrial()
    {
        if (!_trialFile.is_open()) return;
        FlushTrial();
        _trialFile.close();
        if (!_trialFile) throw std::runtime_error("Cannot save Bistro benchmark trial");
    }

    void OpenTrial(std::string_view id)
    {
        CloseTrial();
        _trialId = id;
        const auto filename = "trial-" + _trialId + ".csv";
        const auto path = _directory / filename;
        const bool existing = std::filesystem::exists(path);
        _trialFile.open(path, std::ios::app);
        if (!_trialFile) throw std::runtime_error("Cannot open Bistro benchmark trial");
        if (existing) return;
        _trialFile << Header;
        auto entry = Row("trial");
        entry[Round] = _trialId;
        entry[Name] = "TrialCSV";
        entry[Value] = (_directory.filename() / filename).generic_string();
        Write(_file, entry);
    }

    std::filesystem::path _directory;
    size_t _pass = 0;
    std::string _trialId;
    std::ostringstream _trialRows;
    std::ofstream _file, _trialFile;
};
}

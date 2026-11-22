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
        _file.clear();
        _file.open(path);
        Check();
        _file << "type,stage,mask,section,frame,route_seconds,frame_ms,frame_interval_ms,"
                 "window_start,window_end,frames,fps,mspf,min_fps,min_mspf,max_fps,max_mspf,"
                 "mean_ms,p95_ms,measured_seconds,wall_seconds,status,name,value,round\n";
    }

    void Config(std::string_view name, std::string_view value)
    {
        auto row = Row("config");
        row[Name] = name;
        row[Value] = value;
        Write(_file, row);
    }

    template<class SectionName>
    void Leg(const char* stage, unsigned mask, const std::vector<Frame>& frames, double begin,
        double end, double elapsed, const char* status, SectionName sectionName, unsigned round)
    {
        // Build the rows in memory, then append them in one write outside frame measurement.
        std::ostringstream output;
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
        Summary(output, stage, mask, sectionName(frames.empty() ? 0 : frames.front().Section), begin, end,
            elapsed, Measure(frames, begin, end), status, round);
        _file << output.str();
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

    std::ofstream _file;
};
}

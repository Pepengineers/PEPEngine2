#include "BistroComparePanel.h"
#include "BistroPlot.h"
#include "Techniques/CloudPictureQuality.h"
#include "Common/RuntimePaths.h"
#include "Engine.Core/BenchmarkEngine.h"
#include "Engine.Core/ConsoleModule.h"
#include <imgui/imgui.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <chrono>
#include <ctime>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <optional>

namespace
{
namespace fs = std::filesystem;
using Settings = std::map<std::string, std::string>;

// Read the quoted fields emitted by BistroBenchmarkData::Csv, including embedded newlines.
class CsvReader
{
  public:
    explicit CsvReader(const fs::path &path) : _input(path)
    {
        if (Next())
            for (size_t i = 0; i < _fields.size(); ++i) _columns.emplace(_fields[i], i);
    }
    bool Next()
    {
        _fields.clear();
        std::string line;
        if (!std::getline(_input, line)) return false;
        std::string field;
        bool quoted = false;
        for (;;)
        {
            for (size_t i = 0; i < line.size(); ++i)
            {
                char ch = line[i];
                if (ch == '"')
                {
                    if (quoted && i + 1 < line.size() && line[i + 1] == '"')
                    {
                        field += ch;
                        ++i;
                    }
                    else
                        quoted = !quoted;
                }
                else if (ch == ',' && !quoted)
                {
                    _fields.push_back(std::move(field));
                    field.clear();
                }
                else
                    field += ch;
            }
            if (!quoted) break;
            field += '\n';
            if (!std::getline(_input, line)) return false;
        }
        _fields.push_back(std::move(field));
        return true;
    }
    const std::string &Get(const char *key) const
    {
        static const std::string empty;
        const auto it = _columns.find(key);
        return it != _columns.end() && it->second < _fields.size() ? _fields[it->second] : empty;
    }

  private:
    std::ifstream _input;
    std::map<std::string, size_t, std::less<>> _columns;
    std::vector<std::string> _fields;
};

std::optional<double> Number(const std::string &s)
{
    double v;
    auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    if (r.ec != std::errc{} || r.ptr != s.data() + s.size() || !std::isfinite(v)) return {};
    return v;
}

void ReadSetting(Settings &settings, const std::string &name, const std::string &value)
{
    static const std::array keys{
        "OutputResolution",        "RenderResolution",    "EffectiveUpscaler", "Render.Upscaler.Provider",
        "Render.Upscaler.Quality", "PaintingHoldSeconds", "RouteSeconds",      "WarmupSeconds"};
    if (name.rfind("Render.", 0) == 0 || name.rfind("App.UI.", 0) == 0 || name == "EffectiveFrameGeneration" || std::find(keys.begin(), keys.end(), name) != keys.end()) settings[name] = value;
}
const std::string& Value(const Settings& settings, const std::string& key)
{
    static const std::string empty;
    const auto it = settings.find(key);
    return it == settings.end() ? empty : it->second;
}

std::string DisplayValue(const std::string& key, const std::string& value)
{
    if (value.empty()) return "--";
    if (value == "true") return "On";
    if (value == "false") return "Off";
    auto name = key == "EffectiveUpscaler" ? "Render.Upscaler.Provider" :
        key == "EffectiveFrameGeneration" ? "Render.FrameGeneration.Provider" : key;
    IConsoleVariable* variable{};
    int id{};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), id);
    if (parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() &&
        BenchmarkEngine::GetLocator().GetModule<ConsoleModule>()->TryFindConsoleVariable(fs::u8path(name).wstring(), variable))
    {
        const auto label = variable->GetEnumValueName(id);
        if (key == "App.UI.CloudPicture.Quality" && id >= 0 && size_t(id) < BistroRender::CloudPictureSizes.size())
        {
            const auto [width, height] = BistroRender::CloudPictureSizes[id];
            return std::string(label) + " (" + std::to_string(width) + "x" + std::to_string(height) + ")";
        }
        if (!label.empty()) return std::string(label);
    }
    return value;
}

void DrawSettings(const Settings& values, const Settings& before)
{
    const auto display = [&](const char* key) { return DisplayValue(key, Value(values, key)); };
    const auto provider = display("EffectiveUpscaler");
    if (values.count("OutputResolution"))
    {
        const auto quality = Value(values, "EffectiveUpscaler") == "0" ? "" : " / " + display("Render.Upscaler.Quality");
        ImGui::TextWrapped("%s%s  |  %s render -> %s output", provider.c_str(), quality.c_str(),
            display("RenderResolution").c_str(), display("OutputResolution").c_str());
    }
    const auto custom = std::to_string(int(BistroRender::CloudPictureQuality::Custom));
    const auto table = [&](bool changes)
    {
        if (!ImGui::BeginTable(changes ? "Changes" : "Settings", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) return;
        ImGui::TableSetupColumn("Setting", ImGuiTableColumnFlags_WidthStretch, 2);
        ImGui::TableSetupColumn("Before run");
        ImGui::TableSetupColumn("Selected");
        ImGui::TableHeadersRow();
        size_t count = 0;
        for (const auto& [key, value] : values)
        {
            if (key == "RouteSeconds" || key == "WarmupSeconds" || key == "PaintingHoldSeconds") continue;
            if ((key == "App.UI.CloudPicture.Width" || key == "App.UI.CloudPicture.Height") &&
                values.count("App.UI.CloudPicture.Quality") &&
                Value(values, "App.UI.CloudPicture.Quality") != custom) continue;
            const auto& old = Value(before, key);
            if (changes)
            {
                if (old.empty() || old == value) continue;
                const auto numeric = [](const std::string& raw) {
                    return Number(raw == "true" ? "1" : raw == "false" ? "0" : raw);
                };
                if (const auto a = numeric(old); a && a == numeric(value)) continue;
            }
            ++count;
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", key.c_str() + (key.rfind("Render.", 0) == 0 ? 7 : 0));
            ImGui::TableNextColumn(); ImGui::TextUnformatted(DisplayValue(key, old).c_str());
            ImGui::TableNextColumn();
            auto selected = DisplayValue(key, value);
            const bool scale = key.find("Quality") != key.npos || key.find("Resolution") != key.npos;
            const bool customPicture = key == "App.UI.CloudPicture.Quality" && (old == custom || value == custom);
            if (changes && scale && !customPicture && key != "Render.Upscaler.Quality")
            {
                const auto from = Number(old.substr(0, old.find('x')));
                const auto to = Number(value.substr(0, value.find('x')));
                if (from && to && *from != *to)
                    selected += *to > *from ? " (higher)" : " (lower)";
            }
            ImGui::TextUnformatted(selected.c_str());
        }
        ImGui::EndTable();
        if (changes && !count) ImGui::TextDisabled("No recorded differences.");
    };
    if (before.empty()) ImGui::TextDisabled("Pre-run settings were not recorded.");
    else if (ImGui::CollapsingHeader("Changes from pre-run settings", ImGuiTreeNodeFlags_DefaultOpen)) table(true);
    if (ImGui::CollapsingHeader("All settings")) table(false);
}

std::string RunMode(const std::string &mode)
{
    if (mode == "manual_placement") return "Manual";
    if (mode == "multi_gpu_profiles") return "MGPU";
    if (mode == "single_gpu_profiles") return "Single GPU";
    if (mode.find("search") != mode.npos) return "Async";
    return "Unknown";
}
ImU32 Color(const std::string &mode)
{
    if (mode == "Manual") return IM_COL32(92, 224, 166, 255);
    if (mode == "Async") return IM_COL32(255, 184, 61, 255);
    if (mode == "Single GPU") return IM_COL32(92, 191, 255, 255);
    if (mode == "MGPU") return IM_COL32(217, 145, 255, 255);
    return IM_COL32(200, 200, 200, 255);
}
}

void BistroComparePanel::Refresh()
{
    std::array<fs::path, 2> previous;
    for (size_t i = 0; i < previous.size(); ++i)
    {
        const auto &s = _selected[i];
        if (s.PassIndex >= 0) previous[i] = _runs[s.RunIndex].Passes[s.PassIndex].Path;
    }
    _runs.clear();
    _notice.clear();
    const auto root = Engine::Core::GetExecutableDirectory() / "BenchmarkResults";
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec))
    {
        const auto &file = it->path();
        const auto name = file.filename().u8string();
        if (name.rfind("bistro-", 0) != 0) continue;
        if (it->is_directory(ec))
        {
            it.disable_recursion_pending();
            continue;
        }
        if (file.extension() != ".csv") continue;
        Run run;
        run.Path = file;
        CsvReader csv(file);
        while (csv.Next())
        {
            const auto &key = csv.Get("name");
            const auto &value = csv.Get("value");
            if (csv.Get("type") == "config")
            {
                if (key == "Mode")
                    run.Mode = RunMode(value);
                else if (key == "Profile")
                    run.Profile = value;
                else if (key.rfind("Before.", 0) == 0) run.Before[key.substr(7)] = value;
                else if (key.rfind("MaskBit", 0) == 0) run.MaskBits[key.substr(7)] = value;
                else if (key.rfind("Trial.", 0) == 0)
                {
                    auto dot = key.find('.', 6);
                    if (dot != key.npos && key.substr(dot + 1) == "LastPassCSV")
                        run.TrialPasses[key.substr(6, dot - 6)] = value;
                }
                else if (key.rfind("Native.", 0) == 0 || key.rfind("Upscaled.", 0) == 0)
                {
                    const bool upscaled = key.front() == 'U';
                    const auto prefix = (upscaled ? "Upscaled." : "Native.") + run.Profile + ".";
                    if (key.rfind(prefix, 0) != 0) continue;
                    const auto field = key.substr(prefix.size());
                    auto& winner = run.Winners[upscaled];
                    if (field.rfind("Preset.", 0) == 0) winner.Settings[field.substr(7)] = value;
                    else if (field.find('.') == field.npos) winner.Fields[field] = value;
                }
                else ReadSetting(run.Settings, key, value);
            }
            else if (csv.Get("type") == "result") run.SelectedMask = csv.Get("mask");
            else if (key == "PassCSV" && csv.Get("type") == "pass" && csv.Get("status") == "completed")
            {
                const auto relative = fs::u8path(value);
                if (relative.parent_path() != file.stem()) continue;
                run.Passes.push_back(
                    {file.parent_path() / relative,
                     relative.stem().string() + "  /  " + csv.Get("stage") + "  /  " + csv.Get("section"),
                     csv.Get("mask"), csv.Get("section") == "Full route"});
            }
        }
        if (run.Passes.empty() && run.Winners[0].Fields.empty() && run.Winners[1].Fields.empty()) continue;
        run.Label = run.Mode + (run.Profile.empty() ? "" : " / " + run.Profile) + "  |  " + file.stem().string();
        // The writer places system_clock ticks after the bistro- prefix in each run ID.
        const auto stem = file.stem().string();
        const auto dash = stem.find('-', 7);
        long long ticks{};
        if (dash != stem.npos && std::from_chars(stem.data() + 7, stem.data() + dash, ticks).ec == std::errc{})
        {
            using Clock = std::chrono::system_clock;
            auto t = Clock::to_time_t(Clock::time_point(Clock::duration(ticks)));
            std::tm local{};
            char date[32];
            if (localtime_s(&local, &t) == 0 && std::strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", &local))
                run.Label = run.Mode + (run.Profile.empty() ? "" : " / " + run.Profile) + "  |  " + date;
        }
        _runs.push_back(std::move(run));
    }
    if (ec && ec != std::errc::no_such_file_or_directory) _notice = ec.message();
    std::sort(_runs.begin(), _runs.end(),
              [](const Run &a, const Run &b) { return a.Path.filename() > b.Path.filename(); });
    // A winning MGPU reference can point to a trial measured in a different SingleGPU run.
    for (auto& run : _runs)
        for (auto& winner : run.Winners)
        {
            if (Value(winner.Fields, "Trial").empty()) continue;
            winner.Settings["OutputResolution"] = Value(winner.Fields, "Output");
            winner.Settings["RenderResolution"] = Value(winner.Fields, "Render");
            winner.Settings["EffectiveUpscaler"] = Value(winner.Fields, "Provider");
            winner.Settings["Render.FrameGeneration.Provider"] = "0";
            const auto& source = Value(winner.Fields, "CSV");
            const auto path = source.empty() ? run.Path : (run.Path.parent_path() / fs::u8path(source)).lexically_normal();
            for (int r = 0; r < int(_runs.size()); ++r)
            {
                const auto& measured = _runs[r];
                if (measured.Path != path) continue;
                const auto pass = measured.Path.parent_path() / fs::u8path(Value(measured.TrialPasses, Value(winner.Fields, "Trial")));
                for (int i = 0; i < int(measured.Passes.size()); ++i)
                    if (measured.Passes[i].FullRoute && measured.Passes[i].Path == pass)
                    {
                        winner.RunIndex = r;
                        winner.PassIndex = i;
                    }
                break;
            }
        }
    _completed.clear();
    _lastPass.clear();
    _openError.clear();
    for (int r = 0; r < int(_runs.size()); ++r)
    {
        const auto& run = _runs[r];
        auto& entries = _completed[{run.Mode, (run.Mode == "Single GPU" || run.Mode == "MGPU") ? run.Profile : ""}];
        for (int p = 0; p < int(run.Passes.size()); ++p)
        {
            std::error_code timeError;
            auto time = fs::last_write_time(run.Passes[p].Path, timeError);
            if (!timeError) entries.push_back({time, r, p});
        }
    }
    for (auto& [filter, entries] : _completed)
        std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
            if (a.Completed != b.Completed) return a.Completed > b.Completed;
            if (a.RunIndex != b.RunIndex) return a.RunIndex < b.RunIndex;
            return a.PassIndex > b.PassIndex;
        });
    _loadComparison = true;
    for (int i = 0; i < 2; ++i)
    {
        int r = _runs.empty() ? -1 : std::min(i, int(_runs.size()) - 1);
        int p = r < 0 ? -1 : int(_runs[r].Passes.size()) - 1;
        if (i && r == 0 && p > 0) --p;
        for (int j = 0; j < int(_runs.size()); ++j)
            for (int k = 0; k < int(_runs[j].Passes.size()); ++k)
                if (_runs[j].Passes[k].Path == previous[i])
                {
                    r = j;
                    p = k;
                }
        _selected[i] = {};
        _selected[i].RunIndex = r;
        _selected[i].PassIndex = p;
    }
}

void BistroComparePanel::Select(Selection &s, int run, int pass)
{
    s = {};
    s.RunIndex = run;
    s.PassIndex = pass;
    if (run < 0 || pass < 0) return;
    const auto &r = _runs[run];
    s.Settings = r.Settings;
    s.Mode = r.Mode;
    const auto &path = r.Passes[pass].Path;
    CsvReader csv(path);
    std::string trial;
    bool complete = false;
    std::vector<double> intervals;
    double sum = 0;
    while (csv.Next())
    {
        if (csv.Get("type") == "frame")
        {
            auto t = Number(csv.Get("route_seconds")), ms = Number(csv.Get("frame_interval_ms"));
            if (!t || !ms || *ms <= 0 || (!s.Samples.empty() && *t < s.Samples.back().RouteSeconds))
            {
                s.Error = "Invalid frame samples in " + path.filename().string();
                return;
            }
            size_t number{};
            const auto& index = csv.Get("frame");
            const auto parsed = std::from_chars(index.data(), index.data() + index.size(), number);
            if (parsed.ec != std::errc{} || parsed.ptr != index.data() + index.size()) number = s.Samples.size() + 1;
            s.Samples.push_back({*t, *ms, Number(csv.Get("frame_ms")).value_or(-1), number, csv.Get("section")});
            intervals.push_back(*ms);
            sum += *ms;
            s.MaxMs = std::max(s.MaxMs, *ms);
            s.MaxFps = std::max(s.MaxFps, 1000 / *ms);
        }
        else if (csv.Get("name") == "RunMode")
            s.Mode = csv.Get("value");
        else if (csv.Get("name") == "TrialCSV")
            trial = csv.Get("value");
        else if (csv.Get("type") == "pass" && csv.Get("status") == "completed")
            complete = true;
    }
    if (!complete || intervals.empty())
    {
        s.Error = "Missing or incomplete pass CSV: " + path.filename().string();
        return;
    }
    if (!trial.empty() && fs::u8path(trial).filename() == fs::u8path(trial))
    {
        CsvReader config(path.parent_path() / fs::u8path(trial));
        while (config.Next())
        {
            const auto &key = config.Get("name");
            const auto dot = key.find('.', 6);
            if (key.rfind("Trial.", 0) == 0 && dot != key.npos)
                ReadSetting(s.Settings, key.substr(dot + 1), config.Get("value"));
        }
    }
    if (r.Mode == "Async")
    {
        unsigned mask{};
        const auto& encoded = r.Passes[pass].Mask;
        if (std::from_chars(encoded.data(), encoded.data() + encoded.size(), mask).ec == std::errc{})
            for (unsigned bit = 0; bit < 32; ++bit)
            {
                const auto& name = Value(r.MaskBits, std::to_string(bit));
                if (!name.empty()) s.Settings[name] = mask & (1u << bit) ? "true" : "false";
            }
    }
    s.Fps = 1000 * intervals.size() / sum;
    const auto p95 = intervals.begin() + (intervals.size() * 95 - 1) / 100;
    std::nth_element(intervals.begin(), p95, intervals.end());
    s.P95 = *p95;
    // Keep local extrema in chronological order for rendering, raw samples for the cursor.
    const double start = s.Samples.front().RouteSeconds;
    const double span = std::max(.001, s.Samples.back().RouteSeconds - start);
    const auto bin = [&](size_t i) { return int(512 * (s.Samples[i].RouteSeconds - start) / span); };
    for (size_t first = 0; first < s.Samples.size();)
    {
        size_t lo = first, hi = first, next = first + 1;
        const int bucket = bin(first);
        for (; next < s.Samples.size() && bin(next) == bucket; ++next)
        {
            if (s.Samples[next].IntervalMs < s.Samples[lo].IntervalMs) lo = next;
            if (s.Samples[next].IntervalMs > s.Samples[hi].IntervalMs) hi = next;
        }
        s.Trace.push_back(std::min(lo, hi));
        if (lo != hi) s.Trace.push_back(std::max(lo, hi));
        first = next;
    }
}

void BistroComparePanel::DrawSelection(Selection &s, const char *label)
{
    ImGui::PushID(label);
    ImGui::TextUnformatted(label);
    int r = s.RunIndex;
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##run", r >= 0 ? _runs[r].Label.c_str() : "Choose run"))
    {
        for (int i = 0; i < int(_runs.size()); ++i)
        {
            ImGui::PushID(i);
            if (ImGui::Selectable(_runs[i].Label.c_str(), r == i) && r != i)
                Select(s, i, int(_runs[i].Passes.size()) - 1);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", _runs[i].Path.u8string().c_str());
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    r = s.RunIndex;
    if (r >= 0)
    {
        const auto &passes = _runs[r].Passes;
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##pass", s.PassIndex < 0 ? "No completed pass" : passes[s.PassIndex].Label.c_str()))
        {
            for (int i = int(passes.size()) - 1; i >= 0; --i)
                if (ImGui::Selectable(passes[i].Label.c_str(), s.PassIndex == i) && s.PassIndex != i) Select(s, r, i);
            ImGui::EndCombo();
        }
        if (s.PassIndex >= 0 && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", passes[s.PassIndex].Path.u8string().c_str());
    }
    if (!s.Error.empty()) ImGui::TextWrapped("%s", s.Error.c_str());
    ImGui::PopID();
}

void BistroComparePanel::DrawPlot(const Selection* series, size_t count, bool fps, double target)
{
    const auto &a = series[0];
    const auto &b = series[count - 1];
    if (!a.Error.empty() || !b.Error.empty() || a.Samples.empty() || b.Samples.empty()) return;
    const double begin = std::min(a.Samples.front().RouteSeconds, b.Samples.front().RouteSeconds);
    const double end = std::max(a.Samples.back().RouteSeconds, b.Samples.back().RouteSeconds);
    const double span = std::max(.001, end - begin);
    const double peak = std::max(target > 0 ? (fps ? target : 1000. / target) : 0., fps ? std::max(a.MaxFps, b.MaxFps) : std::max(a.MaxMs, b.MaxMs));
    const auto format = [](double value, const char *fmt) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), fmt, value);
        return std::string(buf);
    };
    if (count == 2)
    {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(Color(a.Mode)), "A   %s", a.Mode.c_str());
        ImGui::SameLine();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(Color(b.Mode)), "B   %s%s", b.Mode.c_str(),
                           a.Mode == b.Mode ? "  (dashed)" : "");
    }
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = std::max(1.f, ImGui::GetContentRegionAvail().x - 66);
    const float height = count == 2 ? 215.f : std::clamp(ImGui::GetContentRegionAvail().y - 30.f, 85.f, 175.f);
    const ImVec2 top{origin.x + 40, origin.y + 10};
    auto *draw = ImGui::GetWindowDrawList();
    const auto muted = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    for (int i = 0; i <= 4; ++i)
    {
        const float f = i * .25f, y = top.y + height * (1 - f);
        draw->AddLine({top.x, y}, {top.x + width, y}, ImGui::GetColorU32(ImGuiCol_Border));
        draw->AddText({origin.x, y - 7}, muted, format(peak * f, "%.1f").c_str());
        const auto tick = format(begin + f * span, "%.1fs");
        draw->AddText({top.x + width * f - ImGui::CalcTextSize(tick.c_str()).x / 2, top.y + height + 4}, muted,
                      tick.c_str());
    }
    if (target > 0)
    {
        const float y = top.y + height * float(1 - (fps ? target : 1000. / target) / peak);
        for (float x = 0; x < width; x += 10)
            draw->AddLine({top.x + x, y}, {top.x + std::min(x + 6, width), y}, muted);
    }
    const auto point = [&](const Selection::Sample &sample) {
        const double value = fps ? 1000 / sample.IntervalMs : sample.IntervalMs;
        return ImVec2{top.x + width * float((sample.RouteSeconds - begin) / span),
                      top.y + height * float(1 - value / peak)};
    };
    for (size_t iSeries = 0; iSeries < count; ++iSeries)
    {
        const auto& s = series[iSeries];
        const auto color = Color(s.Mode);
        ImVec2 previous = point(s.Samples[s.Trace.front()]);
        draw->AddCircleFilled(previous, 2, color);
        float phase = 0;
        for (size_t i = 1; i < s.Trace.size(); ++i)
        {
            const auto p = point(s.Samples[s.Trace[i]]);
            if (count == 2 && &s == &b && a.Mode == b.Mode)
            {
                const float dx = p.x - previous.x, dy = p.y - previous.y, length = std::hypot(dx, dy);
                for (float d = 0; d < length;)
                {
                    const float step = std::min(length - d, (phase < 6 ? 6.f : 10.f) - phase);
                    if (phase < 6)
                        draw->AddLine({previous.x + dx * d / length, previous.y + dy * d / length},
                                      {previous.x + dx * (d + step) / length, previous.y + dy * (d + step) / length},
                                      color, 1.6f);
                    d += step;
                    phase = std::fmod(phase + step, 10.f);
                }
            }
            else
                draw->AddLine(previous, p, color, 1.6f);
            previous = p;
        }
    }
    ImGui::SetCursorScreenPos(top);
    ImGui::InvisibleButton("##compare cursor", {width, height});
    if (ImGui::IsItemHovered())
    {
        const float x = std::clamp(ImGui::GetMousePos().x, top.x, top.x + width);
        const double t = begin + (x - top.x) / width * span;
        draw->AddLine({x, top.y}, {x, top.y + height}, muted);
        ImGui::BeginTooltip();
        ImGui::Text("Route %.2f s", t);
        for (size_t iSeries = 0; iSeries < count; ++iSeries)
        {
            const auto& s = series[iSeries];
            const char id = &s == &a ? 'A' : 'B';
            if (t < s.Samples.front().RouteSeconds || t > s.Samples.back().RouteSeconds)
            {
                ImGui::Text("%c   --", id);
                continue;
            }
            const auto& sample = s.Samples[BistroPlot::NearestFrame(s.Samples, t)];
            const auto pos = point(sample);
            draw->AddCircleFilled(pos, 4, Color(s.Mode));
            draw->AddCircle(pos, 4, muted);
            ImGui::Separator();
            if (count == 2) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(Color(s.Mode)), "%c   %s", id, s.Mode.c_str());
            BistroPlot::DrawFrameDetails(sample.Number, sample.RouteSeconds, sample.Section.c_str(), sample.FrameMs,
                                         sample.IntervalMs);
        }
        ImGui::EndTooltip();
    }
    ImGui::Dummy({0, 20});
    if (count == 2 && ImGui::BeginTable("Compare metrics", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH))
    {
        for (auto title : {"Metric", "A", "B", "B vs A"}) ImGui::TableSetupColumn(title);
        ImGui::TableHeadersRow();
        const auto metric = [&](const char *label, double av, double bv, const std::string &delta) {
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(label);
            ImGui::TableNextColumn();
            ImGui::Text("%.2f", av);
            ImGui::TableNextColumn();
            ImGui::Text("%.2f", bv);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(delta.c_str());
        };
        metric("Average FPS", a.Fps, b.Fps, format(100 * (b.Fps / a.Fps - 1), "%+.1f%%"));
        metric("P95 (ms)", a.P95, b.P95, format(b.P95 - a.P95, "%+.2f ms"));
        ImGui::EndTable();
    }
}

void BistroComparePanel::UpdateHistory(const fs::path& latestCompleted, bool force)
{
    if (force || !_indexed || latestCompleted != _latestCompleted)
    {
        Refresh();
        _indexed = true;
        _latestCompleted = latestCompleted;
    }
}

fs::file_time_type BistroComparePanel::LatestTime(const Filter& filter) const
{
    const auto it = _completed.find(filter);
    return it != _completed.end() && !it->second.empty() ? it->second.front().Completed : fs::file_time_type::min();
}

bool BistroComparePanel::LatestQuality(const char* mode) const
{
    return LatestTime({mode, "Quality"}) > LatestTime({mode, "Performance"});
}

bool BistroComparePanel::LatestSingleGpuIsAsync() const
{
    return LatestTime({"Async", ""}) > std::max(LatestTime({"Single GPU", "Performance"}), LatestTime({"Single GPU", "Quality"}));
}

void BistroComparePanel::DrawLatest(const char* mode, const char* profile, bool fps, bool canApply,
    fs::path& pendingPreset, std::pair<uint16_t, uint16_t>& pendingSize)
{
    const Filter key{mode, profile};
    const auto found = std::find_if(_runs.begin(), _runs.end(), [&](const Run& run) {
        return run.Mode == mode && run.Profile == profile;
    });
    if (found == _runs.end())
    {
        ImGui::TextDisabled("No completed pass for this selection yet.");
        return;
    }
    const Run& run = *found;
    int measuredRun = int(found - _runs.begin());
    int pass = int(run.Passes.size()) - 1;
    const Winner* winner = nullptr;
    const auto confirmed = [](const Winner& candidate) {
        return Value(candidate.Fields, "Status").rfind("confirmed", 0) == 0;
    };
    const bool native = confirmed(run.Winners[0]), upscaled = confirmed(run.Winners[1]);
    if (native || upscaled)
    {
        bool useUpscaled = !native || (upscaled && _upscaled);
        if (native && upscaled)
        {
            if (ImGui::RadioButton("Native##winner", !useUpscaled)) _upscaled = useUpscaled = false;
            ImGui::SameLine();
            if (ImGui::RadioButton("Upscaled##winner", useUpscaled)) _upscaled = useUpscaled = true;
        }
        winner = &run.Winners[useUpscaled];
        measuredRun = winner->RunIndex;
        pass = winner->PassIndex;
    }
    else if (!run.SelectedMask.empty())
    {
        pass = -1;
        for (int i = 0; i < int(run.Passes.size()); ++i)
            if (run.Passes[i].FullRoute && run.Passes[i].Mask == run.SelectedMask) pass = i;
        ImGui::TextWrapped("Selected async queue mask: %s", run.SelectedMask.c_str());
    }
    else if (*profile) ImGui::TextDisabled("No confirmed preset. Showing the last measured pass.");

    auto [it, inserted] = _lastPass.try_emplace(key);
    auto& selection = it->second;
    if (inserted || selection.RunIndex != measuredRun || selection.PassIndex != pass)
        Select(selection, measuredRun, pass);

    ImGui::TextWrapped("%s", run.Label.c_str());
    if (!selection.Samples.empty() && selection.Error.empty())
    {
        const auto& measured = _runs[measuredRun].Passes[pass];
        ImGui::TextWrapped("%s  |  %.1f FPS avg  |  %.1f ms P95", measured.Label.c_str(), selection.Fps, selection.P95);
        if (ImGui::SmallButton("Open CSV"))
            _openError = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", measured.Path.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32
                ? "Could not open the pass CSV." : "";
        if (!_openError.empty()) ImGui::TextUnformatted(_openError.c_str());
        DrawPlot(&selection, 1, fps, winner ? (run.Profile == "Performance" ? 60 : 30) : 0);
    }
    else ImGui::TextWrapped("%s", selection.Error.empty() ? "Full-route frame trace unavailable." : selection.Error.c_str());

    if (winner)
    {
        ImGui::Separator();
        ImGui::Text("Selected %s preset", winner == &run.Winners[0] ? "native" : "upscaled");
        const auto& gpu = Value(winner->Fields, "Winner");
        if (!gpu.empty()) { ImGui::SameLine(); ImGui::TextDisabled("(%s)", gpu.c_str()); }
        const auto file = fs::u8path(Value(winner->Fields, "PresetFile"));
        const auto expected = run.Path.stem().wstring() + (winner == &run.Winners[0] ? L"-native.ini" : L"-upscaled.ini");
        unsigned width{}, height{};
        if (file == expected && sscanf_s(Value(winner->Fields, "Output").c_str(), "%ux%u", &width, &height) == 2 &&
            width && height && width <= UINT16_MAX && height <= UINT16_MAX)
        {
            ImGui::SameLine();
            ImGui::BeginDisabled(!canApply);
            if (ImGui::SmallButton("Apply preset"))
            {
                pendingPreset = run.Path.parent_path() / file;
                pendingSize = {uint16_t(width), uint16_t(height)};
            }
            ImGui::EndDisabled();
        }
        ImGui::TextWrapped("Confirmed route: %.1f FPS avg  |  %.1f ms P95  |  %s  |  %s",
            Number(Value(winner->Fields, "FPS")).value_or(0), Number(Value(winner->Fields, "P95ms")).value_or(0),
            Value(winner->Fields, "Status").c_str(), run.Profile == "Performance" ? "60 FPS target" : "30 FPS target");
        DrawSettings(winner->Settings, run.Before);
    }
    else if (pass >= 0 && selection.Error.empty()) DrawSettings(selection.Settings, run.Before);
    else if (!run.SelectedMask.empty())
    {
        Settings queues;
        unsigned mask{};
        if (std::from_chars(run.SelectedMask.data(), run.SelectedMask.data() + run.SelectedMask.size(), mask).ec == std::errc{})
            for (unsigned bit = 0; bit < 32; ++bit)
                if (const auto& name = Value(run.MaskBits, std::to_string(bit)); !name.empty())
                    queues[name] = mask & (1u << bit) ? "true" : "false";
        DrawSettings(queues, run.Before);
    }
}

void BistroComparePanel::Draw(const fs::path& latestCompleted)
{
    UpdateHistory(latestCompleted, ImGui::SmallButton("Refresh history"));
    if (_loadComparison)
    {
        for (auto& s : _selected) Select(s, s.RunIndex, s.PassIndex);
        _loadComparison = false;
    }
    if (!_notice.empty()) ImGui::TextWrapped("%s", _notice.c_str());
    if (_runs.empty())
    {
        ImGui::TextDisabled("No completed passes in BenchmarkResults.");
        return;
    }
    if (!ImGui::BeginTable("CSV comparison", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) return;
    ImGui::TableSetupColumn("Pass selection", ImGuiTableColumnFlags_WidthStretch, 1);
    ImGui::TableSetupColumn("Chart", ImGuiTableColumnFlags_WidthStretch, 2);
    ImGui::TableNextColumn();
    DrawSelection(_selected[0], "Reference A");
    ImGui::Spacing();
    DrawSelection(_selected[1], "Compare with B");
    ImGui::Separator();
    for (const auto &[key, label] :
         std::initializer_list<std::pair<const char *, const char *>>{{"OutputResolution", "Output"},
                                                                      {"RenderResolution", "Internal render"},
                                                                      {"Render.Upscaler.Provider", "Requested upscaler"},
                                                                      {"EffectiveUpscaler", "Effective upscaler"},
                                                                      {"Render.Upscaler.Quality", "Upscaler quality"},
                                                                      {"RouteSeconds", "Route (s)"},
                                                                      {"PaintingHoldSeconds", "Painting hold (s)"},
                                                                      {"WarmupSeconds", "Warmup (s)"}})
    {
        const auto get = [&](const Selection &s) -> std::string {
            auto it = s.Settings.find(key);
            if (it == s.Settings.end()) return "--";
            return DisplayValue(key, it->second);
        };
        const auto a = get(_selected[0]), b = get(_selected[1]);
        if (a == b)
            ImGui::TextWrapped("%s   %s", label, a.c_str());
        else
            ImGui::TextWrapped("%s   A: %s   B: %s", label, a.c_str(), b.c_str());
    }
    ImGui::TableNextColumn();
    if (ImGui::RadioButton("Frame time (ms)", !_fps)) _fps = false;
    ImGui::SameLine();
    if (ImGui::RadioButton("FPS", _fps)) _fps = true;
    DrawPlot(_selected.data(), _selected.size(), _fps);
    for (int i = 0; i < 2; ++i)
    {
        const auto& selected = _selected[i];
        if (selected.Samples.empty() || !selected.Error.empty()) continue;
        ImGui::PushID(i);
        ImGui::Separator();
        const auto& run = _runs[selected.RunIndex];
        ImGui::Text("%c  |  %s %s  |  Measured settings", 'A' + i, run.Mode.c_str(), run.Profile.c_str());
        for (int n = 0; n < 2; ++n)
        {
            const auto& winner = run.Winners[n];
            if (winner.RunIndex == selected.RunIndex && winner.PassIndex == selected.PassIndex &&
                Value(winner.Fields, "Status").rfind("confirmed", 0) == 0)
                ImGui::Text("Selected %s preset", n ? "upscaled" : "native");
        }
        DrawSettings(selected.Settings, run.Before);
        ImGui::PopID();
    }
    ImGui::EndTable();
}

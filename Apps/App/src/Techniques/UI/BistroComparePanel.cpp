#include "BistroComparePanel.h"
#include "Common/RuntimePaths.h"
#include "Engine.Core/BenchmarkEngine.h"
#include "Engine.Core/ConsoleModule.h"
#include <imgui/imgui.h>
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
    if (std::find(keys.begin(), keys.end(), name) != keys.end()) settings[name] = value;
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
                else
                    ReadSetting(run.Settings, key, value);
            }
            else if (key == "PassCSV" && csv.Get("type") == "pass" && csv.Get("status") == "completed")
            {
                const auto relative = fs::u8path(value);
                if (relative.parent_path() != file.stem()) continue;
                run.Passes.push_back(
                    {file.parent_path() / relative,
                     relative.stem().string() + "  /  " + csv.Get("stage") + "  /  " + csv.Get("section")});
            }
        }
        if (run.Passes.empty()) continue;
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
        Select(_selected[i], r, p);
    }
}

void BistroComparePanel::Select(Selection &s, int run, int pass)
{
    s = {};
    s.RunIndex = run;
    s.PassIndex = pass;
    if (run < 0) return;
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
            if (!t || !ms || *ms <= 0 || (!s.Samples.empty() && *t < s.Samples.back().first))
            {
                s.Error = "Invalid frame samples in " + path.filename().string();
                return;
            }
            s.Samples.emplace_back(*t, *ms);
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
    const auto console = BenchmarkEngine::GetLocator().GetModule<ConsoleModule>();
    for (auto &[key, value] : s.Settings)
    {
        if (key == "EffectiveUpscaler" || key == "Render.Upscaler.Provider" || key == "Render.Upscaler.Quality")
        {
            const auto &setting = console->FindConsoleVariable(
                key == "Render.Upscaler.Quality" ? L"Render.Upscaler.Quality" : L"Render.Upscaler.Provider");
            int id;
            auto parsed = std::from_chars(value.data(), value.data() + value.size(), id);
            if (parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() &&
                id >= setting.GetEnumFirstValue() && id < setting.GetEnumFirstValue() + setting.GetEnumCount())
                value = setting.GetEnumValueName(id);
        }
        else if (auto seconds = Number(value))
        {
            char text[48];
            std::snprintf(text, sizeof(text), "%.6g", *seconds);
            value = text;
        }
    }
    s.Fps = 1000 * intervals.size() / sum;
    const auto p95 = intervals.begin() + (intervals.size() * 95 - 1) / 100;
    std::nth_element(intervals.begin(), p95, intervals.end());
    s.P95 = *p95;
    // Keep local extrema in chronological order for rendering, raw samples for the cursor.
    const double start = s.Samples.front().first;
    const double span = std::max(.001, s.Samples.back().first - start);
    const auto bin = [&](size_t i) { return int(512 * (s.Samples[i].first - start) / span); };
    for (size_t first = 0; first < s.Samples.size();)
    {
        size_t lo = first, hi = first, next = first + 1;
        const int bucket = bin(first);
        for (; next < s.Samples.size() && bin(next) == bucket; ++next)
        {
            if (s.Samples[next].second < s.Samples[lo].second) lo = next;
            if (s.Samples[next].second > s.Samples[hi].second) hi = next;
        }
        s.Trace.push_back(s.Samples[std::min(lo, hi)]);
        if (lo != hi) s.Trace.push_back(s.Samples[std::max(lo, hi)]);
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
        if (ImGui::BeginCombo("##pass", passes[s.PassIndex].Label.c_str()))
        {
            for (int i = int(passes.size()) - 1; i >= 0; --i)
                if (ImGui::Selectable(passes[i].Label.c_str(), s.PassIndex == i) && s.PassIndex != i) Select(s, r, i);
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", passes[s.PassIndex].Path.u8string().c_str());
    }
    if (!s.Error.empty()) ImGui::TextWrapped("%s", s.Error.c_str());
    ImGui::PopID();
}

void BistroComparePanel::DrawPlot() const
{
    const auto &a = _selected[0];
    const auto &b = _selected[1];
    if (!a.Error.empty() || !b.Error.empty() || a.Samples.empty() || b.Samples.empty()) return;
    const double begin = std::min(a.Samples.front().first, b.Samples.front().first);
    const double end = std::max(a.Samples.back().first, b.Samples.back().first);
    const double span = std::max(.001, end - begin);
    const double peak = _fps ? std::max(a.MaxFps, b.MaxFps) : std::max(a.MaxMs, b.MaxMs);
    const auto format = [](double value, const char *fmt) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), fmt, value);
        return std::string(buf);
    };
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(Color(a.Mode)), "A   %s", a.Mode.c_str());
    ImGui::SameLine();
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(Color(b.Mode)), "B   %s%s", b.Mode.c_str(),
                       a.Mode == b.Mode ? "  (dashed)" : "");
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = std::max(1.f, ImGui::GetContentRegionAvail().x - 66);
    constexpr float height = 215;
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
    for (const auto &s : _selected)
    {
        const auto point = [&](const Selection::Sample &sample) {
            const double value = _fps ? 1000 / sample.second : sample.second;
            return ImVec2{top.x + width * float((sample.first - begin) / span),
                          top.y + height * float(1 - value / peak)};
        };
        const auto color = Color(s.Mode);
        ImVec2 previous = point(s.Trace.front());
        draw->AddCircleFilled(previous, 2, color);
        float phase = 0;
        for (size_t i = 1; i < s.Trace.size(); ++i)
        {
            const auto p = point(s.Trace[i]);
            if (&s == &b && a.Mode == b.Mode)
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
        for (const auto &s : _selected)
        {
            const char id = &s == &a ? 'A' : 'B';
            if (t < s.Samples.front().first || t > s.Samples.back().first)
            {
                ImGui::Text("%c   --", id);
                continue;
            }
            auto it = std::lower_bound(s.Samples.begin(), s.Samples.end(), t,
                                       [](const auto &sample, double time) { return sample.first < time; });
            if (it != s.Samples.begin() && t - (it - 1)->first < it->first - t) --it;
            ImGui::Text("%c   %.2f %s", id, _fps ? 1000 / it->second : it->second, _fps ? "FPS" : "ms");
        }
        ImGui::EndTooltip();
    }
    ImGui::Dummy({0, 20});
    if (ImGui::BeginTable("Compare metrics", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH))
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

void BistroComparePanel::Draw(const fs::path& latestCompleted)
{
    if (ImGui::SmallButton("Refresh history") || !_indexed || latestCompleted != _latestCompleted)
    {
        Refresh();
        _indexed = true;
        _latestCompleted = latestCompleted;
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
            return it->second;
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
    DrawPlot();
    ImGui::EndTable();
}

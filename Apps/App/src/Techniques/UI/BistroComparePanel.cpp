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
    std::streamoff Position() { return _input.tellg(); }
    void Seek(std::streamoff offset) { _input.seekg(offset); }
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
    static const std::array keys{"PaintingHoldSeconds", "RouteSeconds", "WarmupSeconds", "RouteVersion", "MeasurementVersion"};
    if (name.rfind("Render.", 0) == 0 || name.rfind("App.", 0) == 0 || std::find(keys.begin(), keys.end(), name) != keys.end())
        settings[name] = value;
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
    auto name = key == "Render.Upscaler.EffectiveProvider" ? "Render.Upscaler.Provider" :
        key == "Render.FrameGeneration.EffectiveProvider" ? "Render.FrameGeneration.Provider" : key;
    IConsoleVariable* variable{};
    int id{};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), id);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) return value;
    if (key.size() >= 8 && key.compare(key.size() - 8, 8, ".Adapter") == 0 && id >= -1)
        return id == -1 ? "Scene GPU" : "GPU " + value;
    if (BenchmarkEngine::GetLocator().GetModule<ConsoleModule>()->TryFindConsoleVariable(fs::u8path(name).wstring(), variable))
    {
        const auto label = variable->GetEnumValueName(id);
        if (key == "Render.CloudPicture.Quality" && id >= 0 && size_t(id) < BistroRender::CloudPictureSizes.size())
        {
            const auto [width, height] = BistroRender::CloudPictureSizes[id];
            return std::string(label) + " (" + std::to_string(width) + "x" + std::to_string(height) + ")";
        }
        if (!label.empty()) return std::string(label);
    }
    // These techniques expose integer CVars with technique-specific publication modes.
    if (key == "Render.Shadows.Sun.CrossSync" ||
        (key.rfind("Render.Shadows.Local.Page", 0) == 0 && key.size() >= 10 && key.compare(key.size() - 10, 10, ".CrossSync") == 0))
    {
        if (id == 1) return "Exact";
        if (id == 3) return "Latest completed";
    }
    if (key == "Render.AO.CrossSync" || key == "Render.Wind.CrossSync" ||
        key == "Render.Particles.CrossSync" || key == "Render.Water.CrossSync" ||
        key == "Render.Sky.SkyView.CrossSync" || key == "Render.WorldCanvas.Atrium.CrossSync" ||
        key == "Render.WorldCanvas.Terrace.CrossSync")
    {
        if (id == 0) return "Previous frame";
        if (id == 1) return "Current frame";
        if (id == 2 && (key == "Render.Water.CrossSync" || key == "Render.Particles.CrossSync")) return "Independent";
        if (id == 3 && key == "Render.Sky.SkyView.CrossSync") return "Latest completed";
        if (id == 3 && key == "Render.Particles.CrossSync") return "Latest completed (frame-driven)";
    }
    if (key == "Render.Sky.AerialPerspective.CrossSync" && id == 1) return "Current frame";
    return value;
}

std::string SettingGroup(std::string_view key)
{
    const auto root = key.find('.');
    if (root == key.npos) return "Benchmark";
    const auto end = key.rfind('.');
    if (root == end) return std::string(key.substr(0, root));
    return std::string(key.substr(root + 1, end - root - 1));
}

std::string SettingLabel(std::string_view key)
{
    if (key == "Provider") return "Requested provider";
    if (key == "CrossSync") return "Cross-GPU synchronization";
    if (key == "Render.Display.OutputResolution") return "Output resolution";
    if (key == "Render.Display.InternalResolution") return "Internal render resolution";
    if (key == "Render.Upscaler.EffectiveProvider") return "Effective upscaler";
    if (key == "Render.FrameGeneration.EffectiveProvider") return "Effective frame generation";
    for (const std::string_view prefix : {"Render.", "App."})
        if (key.substr(0, prefix.size()) == prefix) { key.remove_prefix(prefix.size()); break; }
    const auto lower = [](char c) { return c >= 'a' && c <= 'z'; };
    const auto upper = [](char c) { return c >= 'A' && c <= 'Z'; };
    std::string label;
    for (size_t i = 0; i < key.size(); ++i)
    {
        const char c = key[i];
        if (c == '.') { label += " / "; continue; }
        if (i && upper(c) && (lower(key[i - 1]) ||
            (upper(key[i - 1]) && i + 1 < key.size() && lower(key[i + 1])))) label += ' ';
        label += c;
    }
    return label;
}

std::optional<double> SettingNumber(const std::string& raw)
{
    if (raw == "true") return 1.;
    if (raw == "false") return 0.;
    return Number(raw);
}

enum class Change { None, Higher, Lower, Enabled, Disabled, Changed };

Change SettingChange(const std::string& key, const std::string& before, const std::string& after,
                     const std::string& beforeLabel, const std::string& afterLabel)
{
    if (before.empty() || before == after) return Change::None;
    if (after == "true" || after == "false" || before == "true" || before == "false")
    {
        const auto value = SettingNumber(after);
        if (value == 1.) return Change::Enabled;
        if (value == 0.) return Change::Disabled;
    }
    const auto direction = [](double oldValue, double newValue) {
        if (oldValue == newValue) return Change::Changed;
        return oldValue < newValue ? Change::Higher : Change::Lower;
    };
    if (key == "Render.Display.OutputResolution" || key == "Render.Display.InternalResolution")
    {
        const auto pixels = [](const std::string& s) -> std::optional<double> {
            const auto split = s.find('x');
            if (split == s.npos) return {};
            const auto width = Number(s.substr(0, split)), height = Number(s.substr(split + 1));
            if (!width || !height) return {};
            return *width * *height;
        };
        const auto a = pixels(before), b = pixels(after);
        if (a && b) return direction(*a, *b);
    }
    const std::string_view name(key);
    const auto leaf = name.substr(name.rfind('.') + 1);
    if (leaf == "Quality" && key != "Render.Upscaler.Quality")
    {
        const auto rank = [](std::string_view label) {
            const auto tier = label.substr(0, label.find(' '));
            if (tier == "Off") return 0;
            if (tier == "Low") return 1;
            if (tier == "Mid" || tier == "Medium") return 2;
            if (tier == "High") return 3;
            if (tier == "Epic" || tier == "Ultra") return 4;
            return -1;
        };
        const auto a = rank(beforeLabel), b = rank(afterLabel);
        if (a >= 0 && b >= 0) return direction(a, b);
    }
    if (leaf == "Resolution" || key == "Render.ReflectionProbes.Count" || key == "Render.ReflectionProbes.PrefilterSamples")
    {
        const auto a = Number(before), b = Number(after);
        if (a && b && beforeLabel == before && afterLabel == after) return direction(*a, *b);
    }
    return Change::Changed;
}

void DrawSettings(const Settings& values, const Settings& before, const char* title = "All measured settings",
                  const char* resultColumn = "Measured")
{
    const auto display = [&](const char* key) { return DisplayValue(key, Value(values, key)); };
    if (values.count("Render.Display.OutputResolution"))
    {
        const auto quality = Value(values, "Render.Upscaler.EffectiveProvider") == "0" ? "" : " / " + display("Render.Upscaler.Quality");
        ImGui::TextWrapped("%s%s  |  %s render -> %s output", display("Render.Upscaler.EffectiveProvider").c_str(), quality.c_str(),
            display("Render.Display.InternalResolution").c_str(), display("Render.Display.OutputResolution").c_str());
    }
    const auto table = [&](bool changes)
    {
        std::map<std::string, std::vector<Settings::const_iterator>> groups;
        const auto& pictureQuality = Value(values, "Render.CloudPicture.Quality");
        const auto custom = std::to_string(int(BistroRender::CloudPictureQuality::Custom));
        size_t total = 0;
        for (auto it = values.begin(); it != values.end(); ++it)
        {
            const auto& [key, value] = *it;
            if (key == "RouteSeconds" || key == "WarmupSeconds" || key == "PaintingHoldSeconds") continue;
            if ((key == "Render.CloudPicture.Width" || key == "Render.CloudPicture.Height") &&
                !pictureQuality.empty() && pictureQuality != custom) continue;
            if (changes)
            {
                const auto& old = Value(before, key);
                if (old.empty() || old == value) continue;
                if (const auto a = SettingNumber(old); a && a == SettingNumber(value)) continue;
            }
            groups[SettingGroup(key)].push_back(it);
            ++total;
        }
        if (!total)
        {
            ImGui::TextDisabled("%s", changes ? "No recorded differences." : "No recorded settings.");
            return;
        }
        ImGui::TextDisabled("%zu %s", total, changes ? (total == 1 ? "change" : "changes") :
                                                       (total == 1 ? "setting" : "settings"));
        const float width = std::min(ImGui::GetContentRegionAvail().x, 88.f * ImGui::GetFontSize());
        if (!ImGui::BeginTable(changes ? "Changes" : "Settings", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable,
            {width, 0})) return;
        ImGui::TableSetupColumn("Setting", ImGuiTableColumnFlags_WidthStretch, 1.8f);
        ImGui::TableSetupColumn("Before run", ImGuiTableColumnFlags_WidthStretch, 1.f);
        ImGui::TableSetupColumn(resultColumn, ImGuiTableColumnFlags_WidthStretch, 1.5f);
        ImGui::TableHeadersRow();
        for (const auto& [group, rows] : groups)
        {
            const auto title = SettingLabel(group);
            ImGui::TableNextRow();
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ImGuiCol_Header));
            ImGui::TableSetColumnIndex(0);
            const bool expanded = ImGui::TreeNodeEx(title.c_str(),
                ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth |
                (changes ? ImGuiTreeNodeFlags_DefaultOpen : 0),
                "%s (%zu)", title.c_str(), rows.size());
            if (!expanded) continue;
            for (const auto it : rows)
            {
                const auto& [key, value] = *it;
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextWrapped("%s", SettingLabel(std::string_view(key).substr(key.rfind('.') + 1)).c_str());
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", key.c_str());
                ImGui::TableSetColumnIndex(1);
                const auto& old = Value(before, key);
                const auto oldLabel = DisplayValue(key, old);
                const bool crossSync = key.size() >= 10 && key.compare(key.size() - 10, 10, ".CrossSync") == 0;
                const auto adapter = crossSync ? key.substr(0, key.size() - 10) + ".Adapter" : "";
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::TextWrapped("%s%s", oldLabel.c_str(), crossSync && Value(before, adapter) == "-1" ? " (unused locally)" : "");
                ImGui::PopStyleColor();
                ImGui::TableSetColumnIndex(2);
                const auto selected = DisplayValue(key, value);
                if (crossSync && Value(values, adapter) == "-1")
                {
                    ImGui::TextWrapped("%s (unused locally)", selected.c_str());
                    continue;
                }
                if (!changes) { ImGui::TextWrapped("%s", selected.c_str()); continue; }
                const auto change = SettingChange(key, old, value, oldLabel, selected);
                constexpr const char* descriptions[]{"", "higher", "lower", "enabled", "disabled", "changed"};
                constexpr ImVec4 raised{.52f, .86f, .68f, 1.f}, lowered{.97f, .76f, .48f, 1.f}, changed{.63f, .82f, 1.f, 1.f};
                const auto color = change == Change::Higher || change == Change::Enabled ? raised :
                    change == Change::Lower || change == Change::Disabled ? lowered : changed;
                ImGui::PushStyleColor(ImGuiCol_Text, color);
                ImGui::TextWrapped("%s  (%s)", selected.c_str(), descriptions[size_t(change)]);
                ImGui::PopStyleColor();
            }
        }
        ImGui::EndTable();
    };
    if (before.empty()) ImGui::TextDisabled("Pre-run settings were not recorded.");
    else if (ImGui::CollapsingHeader("Changes from pre-run settings", ImGuiTreeNodeFlags_DefaultOpen)) table(true);
    if (ImGui::CollapsingHeader(title)) table(false);
}

std::string RunMode(const std::string &mode)
{
    if (mode == "manual_placement" || mode == "manual_run") return "Manual";
    if (mode == "multi_gpu_profiles") return "MGPU";
    if (mode == "single_gpu_profiles") return "Single GPU";
    if (mode.find("search") != mode.npos) return "Async";
    return "Unknown";
}
ImU32 RunModeColor(const std::string &mode)
{
    if (mode == "Manual") return IM_COL32(92, 224, 166, 255);
    if (mode == "Async") return IM_COL32(255, 184, 61, 255);
    if (mode == "Single GPU") return IM_COL32(92, 191, 255, 255);
    if (mode == "MGPU") return IM_COL32(217, 145, 255, 255);
    return IM_COL32(200, 200, 200, 255);
}
}

void BistroComparePanel::Refresh(bool force)
{
    std::array<std::pair<fs::path, std::streamoff>, 2> previous;
    for (size_t i = 0; i < previous.size(); ++i)
    {
        const auto &s = _selected[i];
        if (s.PassIndex >= 0)
        {
            const auto& pass = _runs[s.RunIndex].Passes[s.PassIndex];
            previous[i] = {pass.Path, pass.InlineOffset};
        }
    }
    std::map<fs::path, Run> cached;
    if (!force)
        for (auto& run : _runs) cached.try_emplace(run.Path, std::move(run));
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
        std::error_code timeError;
        const auto modified = it->last_write_time(timeError);
        const auto old = cached.find(file);
        if (!timeError && old != cached.end() && old->second.Modified == modified)
        {
            _runs.push_back(std::move(old->second));
            continue;
        }
        Run run;
        run.Path = file;
        run.Modified = modified;
        run.Settings.emplace("MeasurementVersion", "1");
        CsvReader csv(file);
        std::streamoff inlineOffset = csv.Position();
        bool inlineFrames = false;
        const auto appendPass = [&](fs::path path, std::string label, std::streamoff offset)
        {
            Window range{Number(csv.Get("window_start")).value_or(-1), Number(csv.Get("window_end")).value_or(-1),
                         csv.Get("section")};
            // Earlier writers used the first frame's section even for a complete route.
            if (range.Begin == 0 && range.End > 0 && Number(Value(run.Settings, "RouteSeconds")) == range.End)
                range.Section = "Full route";
            const auto& stage = csv.Get("stage");
            if (!stage.empty()) label += "  /  " + stage;
            if (!range.Section.empty() && range.Section != stage) label += "  /  " + range.Section;
            run.Passes.push_back({std::move(path), std::move(label), csv.Get("mask"), std::move(range), offset});
        };
        while (csv.Next())
        {
            const auto &key = csv.Get("name");
            const auto &value = csv.Get("value");
            if (csv.Get("type") == "config")
            {
                if (key == "Mode")
                {
                    run.Mode = RunMode(value);
                    run.AfterRender = run.Mode == "Async" || value == "manual_run";
                }
                else if (key == "Profile")
                    run.Profile = value;
                else if (key == "StopReason") run.StopReason = value;
                else if (key == "FastestMask")
                {
                    const auto mask = Number(value);
                    if (mask && *mask >= 0 && *mask <= UINT32_MAX && *mask == std::floor(*mask))
                        run.FinalMask = std::to_string(uint32_t(*mask));
                }
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
                    if (field.rfind("SingleGPU.", 0) == 0 || field.rfind("MultiGPU.", 0) == 0)
                    {
                        auto& placement = winner.Placements[field.front() == 'M'];
                        const auto key = field.substr(field.find('.') + 1);
                        if (key == "Technique")
                        {
                            const auto split = value.find('=');
                            if (split != value.npos) placement.Techniques[value.substr(0, split)] = value.substr(split + 1);
                        }
                        else if (const auto dot = key.rfind('.'); dot != key.npos &&
                            (key.compare(dot, key.npos, ".EffectiveOwner") == 0 || key.compare(dot, key.npos, ".Effective") == 0))
                            placement.GpuSettings[key.substr(0, dot)] = value;
                        else if (key.find('.') == key.npos) placement.Fields[key] = value;
                    }
                    else if (field.rfind("Preset.", 0) == 0) winner.Settings[field.substr(7)] = value;
                    else if (field.find('.') == field.npos) winner.Fields[field] = value;
                }
                else ReadSetting(run.Settings, key, value);
            }
            else if (csv.Get("type") == "result")
            {
                run.FinalMask = csv.Get("mask");
                run.StopReason = csv.Get("status");
            }
            else if (csv.Get("type") == "frame") inlineFrames = true;
            else if (csv.Get("type") == "summary")
            {
                if (inlineFrames && csv.Get("status") == "completed")
                    appendPass(file, "Pass " + std::to_string(run.Passes.size() + 1), inlineOffset);
                inlineFrames = false;
                inlineOffset = csv.Position();
            }
            else if (key == "PassCSV" && csv.Get("type") == "pass" && csv.Get("status") == "completed")
            {
                const auto relative = fs::u8path(value);
                if (relative.parent_path() != file.stem()) continue;
                appendPass(file.parent_path() / relative, relative.stem().string(), -1);
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
    // A saved reference can link to a trial that reused the full route of another profile.
    for (auto& run : _runs)
        for (auto& winner : run.Winners)
        {
            winner.RunIndex = winner.PassIndex = -1;
            if (Value(winner.Fields, "Trial").empty()) continue;
            winner.Settings["Render.Display.OutputResolution"] = Value(winner.Fields, "Output");
            winner.Settings["Render.Display.InternalResolution"] = Value(winner.Fields, "Render");
            winner.Settings["Render.Upscaler.EffectiveProvider"] = Value(winner.Fields, "Provider");
            winner.Settings["Render.FrameGeneration.Provider"] = "0";
            const auto& source = Value(winner.Fields, "CSV");
            const auto path = source.empty() ? run.Path : (run.Path.parent_path() / fs::u8path(source)).lexically_normal();
            const auto trialRun = std::find_if(_runs.begin(), _runs.end(), [&](const Run& candidate) { return candidate.Path == path; });
            if (trialRun == _runs.end()) continue;
            const auto& lastPass = Value(trialRun->TrialPasses, Value(winner.Fields, "Trial"));
            if (lastPass.empty()) continue;
            const auto pass = (path.parent_path() / fs::u8path(lastPass)).lexically_normal();
            for (int r = 0; r < int(_runs.size()); ++r)
            {
                const auto& measured = _runs[r];
                for (int i = 0; i < int(measured.Passes.size()); ++i)
                    if (measured.Passes[i].Range.Section == "Full route" && measured.Passes[i].Path == pass)
                    {
                        winner.RunIndex = r;
                        winner.PassIndex = i;
                    }
                if (winner.RunIndex >= 0) break;
            }
        }
    _completed.clear();
    _lastPass.clear();
    _openError.clear();
    for (int r = 0; r < int(_runs.size()); ++r)
    {
        const auto& run = _runs[r];
        if (run.Passes.empty()) continue;
        auto [it, added] = _completed.try_emplace({run.Mode, run.Profile}, run.Modified);
        if (!added) it->second = std::max(it->second, run.Modified);
    }
    _loadComparison = true;
    for (int i = 0; i < 2; ++i)
    {
        int r = _runs.empty() ? -1 : std::min(i, int(_runs.size()) - 1);
        int p = r < 0 ? -1 : int(_runs[r].Passes.size()) - 1;
        if (i && r == 0 && p > 0) --p;
        for (int j = 0; j < int(_runs.size()); ++j)
            for (int k = 0; k < int(_runs[j].Passes.size()); ++k)
                if (_runs[j].Passes[k].Path == previous[i].first && _runs[j].Passes[k].InlineOffset == previous[i].second)
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
    const auto& measured = r.Passes[pass];
    s.Range = measured.Range;
    s.AfterRender = r.AfterRender;
    const auto& path = measured.Path;
    CsvReader csv(path);
    if (measured.InlineOffset >= 0) csv.Seek(measured.InlineOffset);
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
        else if (csv.Get("type") == "summary" && !s.Samples.empty())
        {
            s.ScopeMeanMs = Number(csv.Get("mean_ms")).value_or(-1);
            s.ScopeP95Ms = Number(csv.Get("p95_ms")).value_or(-1);
            if (measured.InlineOffset >= 0)
            {
                complete = csv.Get("status") == "completed";
                break;
            }
        }
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
        if (!s.Samples.empty() && s.Error.empty())
            ImGui::TextDisabled("%.3f - %.3f s  |  %s", s.Range.Begin, s.Range.End,
                s.AfterRender ? "after Render / Present" : "before Present");
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
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(RunModeColor(a.Mode)), "A   %s", a.Mode.c_str());
        ImGui::SameLine();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(RunModeColor(b.Mode)), "B   %s%s", b.Mode.c_str(),
                           a.Mode == b.Mode ? "  (dashed)" : "");
    }
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float em = ImGui::GetFontSize();
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const float yLabelWidth = ImGui::CalcTextSize(format(peak, "%.1f").c_str()).x;
    const float timeLabelWidth = ImGui::CalcTextSize(format(end, "%.1fs").c_str()).x;
    const float left = std::max(yLabelWidth + .75f * em, .5f * timeLabelWidth);
    const float width = std::max(1.f, available.x - left - .5f * timeLabelWidth);
    const float height = std::clamp(available.y * .32f, 6.f * em, (count == 2 ? 14.f : 10.f) * em);
    const ImVec2 top{origin.x + left, origin.y + .6f * em};
    const float stroke = std::max(1.f, .12f * em);
    const float dashLength = std::round(.5f * em);
    const float dashPeriod = dashLength + std::max(1.f, std::round(.3f * em));
    auto *draw = ImGui::GetWindowDrawList();
    const auto muted = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    for (int i = 0; i <= 4; ++i)
    {
        const float f = i * .25f, y = top.y + height * (1 - f);
        draw->AddLine({top.x, y}, {top.x + width, y}, ImGui::GetColorU32(ImGuiCol_Border));
        draw->AddText({origin.x, y - .5f * em}, muted, format(peak * f, "%.1f").c_str());
        const auto tick = format(begin + f * span, "%.1fs");
        if ((i == 0 || i == 4) || width >= 4.f * (timeLabelWidth + .5f * em))
            draw->AddText({top.x + width * f - ImGui::CalcTextSize(tick.c_str()).x / 2, top.y + height + .25f * em},
                          muted, tick.c_str());
    }
    if (target > 0)
    {
        const float y = top.y + height * float(1 - (fps ? target : 1000. / target) / peak);
        for (float x = 0; x < width; x += dashPeriod)
            draw->AddLine({top.x + x, y}, {top.x + std::min(x + dashLength, width), y}, muted, stroke);
    }
    const auto point = [&](const Selection::Sample &sample) {
        const double value = fps ? 1000 / sample.IntervalMs : sample.IntervalMs;
        return ImVec2{top.x + width * float((sample.RouteSeconds - begin) / span),
                      top.y + height * float(1 - value / peak)};
    };
    for (size_t iSeries = 0; iSeries < count; ++iSeries)
    {
        const auto& s = series[iSeries];
        const auto color = RunModeColor(s.Mode);
        ImVec2 previous = point(s.Samples[s.Trace.front()]);
        draw->AddCircleFilled(previous, .15f * em, color);
        float phase = 0;
        for (size_t i = 1; i < s.Trace.size(); ++i)
        {
            const auto p = point(s.Samples[s.Trace[i]]);
            if (count == 2 && &s == &b && a.Mode == b.Mode)
            {
                const float dx = p.x - previous.x, dy = p.y - previous.y, length = std::hypot(dx, dy);
                for (float d = 0; d < length;)
                {
                    const float step = std::min(length - d, (phase < dashLength ? dashLength : dashPeriod) - phase);
                    if (phase < dashLength)
                        draw->AddLine({previous.x + dx * d / length, previous.y + dy * d / length},
                                      {previous.x + dx * (d + step) / length, previous.y + dy * (d + step) / length},
                                      color, stroke);
                    d += step;
                    phase = std::fmod(phase + step, dashPeriod);
                }
            }
            else
                draw->AddLine(previous, p, color, stroke);
            previous = p;
        }
    }
    ImGui::SetCursorScreenPos(top);
    ImGui::InvisibleButton("##compare cursor", {width, height});
    if (ImGui::IsItemHovered())
    {
        const float x = std::clamp(ImGui::GetMousePos().x, top.x, top.x + width);
        const double t = begin + (x - top.x) / width * span;
        draw->AddLine({x, top.y}, {x, top.y + height}, muted, stroke);
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
            draw->AddCircleFilled(pos, .3f * em, RunModeColor(s.Mode));
            draw->AddCircle(pos, .3f * em, muted);
            ImGui::Separator();
            if (count == 2) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(RunModeColor(s.Mode)), "%c   %s", id, s.Mode.c_str());
            BistroPlot::DrawFrameDetails(sample.Number, sample.RouteSeconds, sample.Section.c_str(), sample.FrameMs,
                                         sample.IntervalMs);
        }
        ImGui::EndTooltip();
    }
    ImGui::Dummy({0, 1.4f * em});
    if (count != 2) return;
    const auto version = Value(a.Settings, "RouteVersion");
    const bool sameRoute = !version.empty() && version == Value(b.Settings, "RouteVersion") &&
        Number(Value(a.Settings, "RouteSeconds")).value_or(-1) > 0 &&
        Number(Value(a.Settings, "RouteSeconds")) == Number(Value(b.Settings, "RouteSeconds")) &&
        Value(a.Settings, "PaintingHoldSeconds") == Value(b.Settings, "PaintingHoldSeconds");
    const bool sameWindow = !a.Range.Section.empty() && a.Range.Section == b.Range.Section &&
        a.Range.Begin >= 0 && a.Range.End > a.Range.Begin && a.Range.Begin == b.Range.Begin && a.Range.End == b.Range.End;
    const bool comparable = sameRoute && sameWindow && a.AfterRender == b.AfterRender &&
        Value(a.Settings, "MeasurementVersion") == Value(b.Settings, "MeasurementVersion") &&
        a.Mode != "Unknown" && b.Mode != "Unknown";
    if (!comparable)
        ImGui::TextWrapped("A/B deltas require matching routes, measured windows, timing boundaries and measurement versions.");
    if (ImGui::BeginTable("Compare metrics", comparable ? 4 : 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH))
    {
        for (auto title : {"Metric", "A", "B"}) ImGui::TableSetupColumn(title);
        if (comparable) ImGui::TableSetupColumn("B vs A");
        ImGui::TableHeadersRow();
        const auto metric = [&](const char *label, double av, double bv, const std::string &delta) {
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(label);
            ImGui::TableNextColumn();
            ImGui::Text("%.2f", av);
            ImGui::TableNextColumn();
            ImGui::Text("%.2f", bv);
            if (comparable)
            {
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(delta.c_str());
            }
        };
        metric("Application FPS", a.Fps, b.Fps, format(100 * (b.Fps / a.Fps - 1), "%+.1f%%"));
        metric("Frame interval P95 (ms)", a.P95, b.P95, format(b.P95 - a.P95, "%+.2f ms"));
        if (a.ScopeMeanMs > 0 && b.ScopeMeanMs > 0)
            metric("Timed CPU scope mean (ms)", a.ScopeMeanMs, b.ScopeMeanMs, format(b.ScopeMeanMs - a.ScopeMeanMs, "%+.2f ms"));
        if (a.ScopeP95Ms > 0 && b.ScopeP95Ms > 0)
            metric("Timed CPU scope P95 (ms)", a.ScopeP95Ms, b.ScopeP95Ms, format(b.ScopeP95Ms - a.ScopeP95Ms, "%+.2f ms"));
        ImGui::EndTable();
    }
}

void BistroComparePanel::UpdateHistory(const fs::path& latestCompleted, bool force)
{
    if (force || !_indexed || latestCompleted != _latestCompleted)
    {
        Refresh(force);
        _indexed = true;
        _latestCompleted = latestCompleted;
    }
}

fs::file_time_type BistroComparePanel::LatestTime(const Filter& filter) const
{
    const auto it = _completed.find(filter);
    return it != _completed.end() ? it->second : fs::file_time_type::min();
}

bool BistroComparePanel::LatestQuality(const char* mode) const
{
    return LatestTime({mode, "Quality"}) > LatestTime({mode, "Performance"});
}

bool BistroComparePanel::LatestSingleGpuIsAsync() const
{
    return LatestTime({"Async", ""}) > std::max(LatestTime({"Single GPU", "Performance"}), LatestTime({"Single GPU", "Quality"}));
}

void BistroComparePanel::DrawPlacementComparison(const Winner& winner)
{
    const auto& single = winner.Placements[0];
    const auto& multi = winner.Placements[1];
    if (single.Fields.empty() && multi.Fields.empty()) return;
    ImGui::SeparatorText("Single GPU vs Multi-GPU");
    const auto columns = [] {
        ImGui::TableSetupColumn("Full route", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn("Single GPU reference", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Multi-GPU candidate", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
    };
    const auto row = [](const std::string& label, const std::string& a, const std::string& b, bool changed = false) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::TextWrapped("%s", label.c_str());
        ImGui::TableSetColumnIndex(1); ImGui::TextWrapped("%s", a.empty() ? "--" : a.c_str());
        ImGui::TableSetColumnIndex(2);
        if (changed) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{.45f, .82f, 1.f, 1.f});
        ImGui::TextWrapped("%s", b.empty() ? "--" : b.c_str());
        if (changed) ImGui::PopStyleColor();
    };
    const auto flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH;
    const ImVec2 size{std::min(ImGui::GetContentRegionAvail().x, 80.f * ImGui::GetFontSize()), 0};
    if (ImGui::BeginTable("Placement results", 3, flags, size))
    {
        columns();
        for (const auto& [label, field] : std::initializer_list<std::pair<const char*, const char*>>{
                 {"Status", "Status"}, {"Output", "Output"}, {"Internal render", "Render"},
                 {"Application FPS", "FPS"}, {"Frame interval P95 (ms)", "P95ms"}})
            row(label, Value(single.Fields, field), Value(multi.Fields, field));
        row("Upscaler", DisplayValue("Render.Upscaler.EffectiveProvider", Value(single.Fields, "Provider")),
                        DisplayValue("Render.Upscaler.EffectiveProvider", Value(multi.Fields, "Provider")));
        ImGui::EndTable();
    }
    if (const auto gain = Number(Value(winner.Fields, "SameImageFpsGainPct")))
        ImGui::Text("Same-image FPS change  %+.1f%%", *gain);
    if (const auto& explanation = Value(winner.Fields, "Comparison"); !explanation.empty())
        ImGui::TextWrapped("%s", explanation.c_str());

    const bool bothMeasured = !Value(single.Fields, "FPS").empty() && !Value(multi.Fields, "FPS").empty();
    const auto details = [&](bool changes) {
        if (!ImGui::BeginTable(changes ? "Placement differences" : "Placement details", 3, flags, size)) return;
        columns();
        unsigned count = 0;
        const auto group = [&](const Values& a, const Values& b, bool gpu) {
            const auto item = [&](const std::string& key, const std::string& before, const std::string& after) {
                const bool changed = bothMeasured && before != after;
                if (changes && !changed) return;
                ++count;
                row(gpu ? SettingLabel(key) : key, gpu ? DisplayValue(key, before) : before,
                    gpu ? DisplayValue(key, after) : after, changed);
            };
            for (const auto& [key, value] : a) item(key, value, Value(b, key));
            for (const auto& [key, value] : b) if (!a.count(key)) item(key, {}, value);
        };
        group(single.GpuSettings, multi.GpuSettings, true);
        group(single.Techniques, multi.Techniques, false);
        ImGui::EndTable();
        if (changes && !count) ImGui::TextDisabled("No recorded placement or technique changes.");
    };
    if (bothMeasured && ImGui::CollapsingHeader("Placement and technique changes", ImGuiTreeNodeFlags_DefaultOpen))
        details(true);
    if (ImGui::CollapsingHeader("All placement and technique settings")) details(false);
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
    const auto available = [&](const Winner& result) {
        return confirmed(result) || !result.Placements[0].Fields.empty() || !result.Placements[1].Fields.empty();
    };
    const bool native = available(run.Winners[0]), upscaled = available(run.Winners[1]);
    const Winner* placement = nullptr;
    if (native || upscaled)
    {
        bool useUpscaled = !native || (upscaled && _upscaled);
        if (native && upscaled)
        {
            useUpscaled = BistroPlot::Choice("Image mode", "Native", "Upscaled", _upscaled);
            _upscaled = useUpscaled;
        }
        placement = &run.Winners[useUpscaled];
        if (confirmed(*placement))
        {
            winner = placement;
            measuredRun = winner->RunIndex;
            pass = winner->PassIndex;
        }
        else
        {
            pass = -1;
            ImGui::TextDisabled("No confirmed preset for this image mode.");
        }
    }
    else if (!run.FinalMask.empty())
    {
        int matching = -1;
        if (run.StopReason != "cancelled" && run.StopReason != "error")
            for (int i = 0; i < int(run.Passes.size()); ++i)
                if (run.Passes[i].Range.Section == "Full route" && run.Passes[i].Mask == run.FinalMask) matching = i;
        if (matching >= 0) pass = matching;
        else if (pass >= 0) ImGui::TextDisabled("Showing the last completed pass.");
        ImGui::TextWrapped("Final async queue mask: %s  |  Ranked by timed CPU scope (mean and P95)", run.FinalMask.c_str());
    }
    else if (*profile) ImGui::TextDisabled("No confirmed preset. Showing the last measured pass.");

    auto [it, inserted] = _lastPass.try_emplace(key);
    auto& selection = it->second;
    if (inserted || selection.RunIndex != measuredRun || selection.PassIndex != pass)
        Select(selection, measuredRun, pass);

    const double target = run.Profile == "Performance" ? 60. : 30.;
    const bool hasTrace = !selection.Samples.empty() && selection.Error.empty();
    ImGui::TextDisabled("%s", run.Label.c_str());
    if (!winner && !run.StopReason.empty()) ImGui::TextWrapped("Run outcome: %s", run.StopReason.c_str());
    if (hasTrace)
    {
        const auto& measured = _runs[measuredRun].Passes[pass];
        if (!winner)
            ImGui::TextWrapped("%.1f application FPS   |   %.1f ms frame interval P95", selection.Fps, selection.P95);
        if (ImGui::CollapsingHeader(fps ? "Frame rate (FPS)###routePlot" : "Frame interval (ms)###routePlot",
                ImGuiTreeNodeFlags_DefaultOpen))
        {
            if (winner)
                ImGui::TextDisabled("Dashed line: %.0f FPS target (%.1f ms)", target, 1000. / target);
            DrawPlot(&selection, 1, fps, winner ? target : 0.);
        }
        if (ImGui::TreeNode("Run details"))
        {
            ImGui::TextWrapped("%s  |  %.1f application FPS  |  %.1f ms P95", measured.Label.c_str(), selection.Fps, selection.P95);
            ImGui::TextDisabled("Interval measured %s", selection.AfterRender ? "after Render / Present" : "before Present");
            if (selection.Mode == "Async" && selection.ScopeMeanMs > 0 && selection.ScopeP95Ms > 0)
                ImGui::Text("Timed CPU scope  %.2f ms mean  |  %.2f ms P95", selection.ScopeMeanMs, selection.ScopeP95Ms);
            if (winner && !run.StopReason.empty()) ImGui::TextWrapped("Run outcome: %s", run.StopReason.c_str());
            if (ImGui::SmallButton("Open pass CSV"))
                _openError = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", measured.Path.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32
                    ? "Could not open the pass CSV." : "";
            if (!_openError.empty()) ImGui::TextUnformatted(_openError.c_str());
            if (winner && ImGui::TreeNode("Measured pass settings"))
            {
                DrawSettings(selection.Settings, run.Before);
                ImGui::TreePop();
            }
            ImGui::TreePop();
        }
    }
    else if (!placement || winner)
        ImGui::TextWrapped("%s", selection.Error.empty() ? "Full-route frame trace unavailable." : selection.Error.c_str());

    if (placement) DrawPlacementComparison(*placement);
    if (winner)
    {
        ImGui::Spacing();
        ImGui::SeparatorText(winner == &run.Winners[0] ? "Selected preset / Native" : "Selected preset / Upscaled");
        const auto& gpu = Value(winner->Fields, "Winner");
        if (!gpu.empty()) ImGui::Text("Winning placement: %s", gpu.c_str());
        const auto file = fs::u8path(Value(winner->Fields, "PresetFile"));
        const auto expected = run.Path.stem().wstring() + (winner == &run.Winners[0] ? L"-native.ini" : L"-upscaled.ini");
        unsigned width{}, height{};
        if (file == expected && sscanf_s(Value(winner->Fields, "Output").c_str(), "%ux%u", &width, &height) == 2 &&
            width && height && width <= UINT16_MAX && height <= UINT16_MAX)
        {
            if (!gpu.empty()) ImGui::SameLine();
            ImGui::BeginDisabled(!canApply);
            if (ImGui::SmallButton("Apply preset"))
            {
                pendingPreset = run.Path.parent_path() / file;
                pendingSize = {uint16_t(width), uint16_t(height)};
            }
            ImGui::EndDisabled();
        }
        const float widthLimit = 52.f * ImGui::GetFontSize();
        if (ImGui::BeginTable("Confirmed route", 3, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV,
                {std::min(widthLimit, ImGui::GetContentRegionAvail().x), 0}))
        {
            ImGui::TableNextColumn(); ImGui::TextDisabled("Application FPS");
            ImGui::TableNextColumn(); ImGui::TextDisabled("Frame interval P95");
            ImGui::TableNextColumn(); ImGui::TextDisabled("Target: %.0f FPS", target);
            ImGui::TableNextColumn();
            const auto average = Number(Value(winner->Fields, "FPS"));
            if (average) ImGui::Text("%.1f FPS", *average); else ImGui::TextUnformatted("--");
            ImGui::TableNextColumn();
            if (auto p95 = Number(Value(winner->Fields, "P95ms"))) ImGui::Text("%.1f ms", *p95); else ImGui::TextUnformatted("--");
            ImGui::TableNextColumn();
            if (average && *average >= target)
                ImGui::TextColored({.49f,.87f,.73f,1}, "Target met");
            else
                ImGui::TextWrapped("%s", Value(winner->Fields, "Status").c_str());
            ImGui::EndTable();
        }
        DrawSettings(winner->Settings, run.Before, "All preset settings", "Selected preset");
    }
    else if (pass >= 0 && selection.Error.empty()) DrawSettings(selection.Settings, run.Before);
    else if (!run.FinalMask.empty())
    {
        Settings queues;
        unsigned mask{};
        if (std::from_chars(run.FinalMask.data(), run.FinalMask.data() + run.FinalMask.size(), mask).ec == std::errc{})
            for (unsigned bit = 0; bit < 32; ++bit)
                if (const auto& name = Value(run.MaskBits, std::to_string(bit)); !name.empty())
                    queues[name] = mask & (1u << bit) ? "true" : "false";
        DrawSettings(queues, run.Before, "Final queue settings", "Final queues");
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
         std::initializer_list<std::pair<const char *, const char *>>{{"Render.Display.OutputResolution", "Output"},
                                                                      {"Render.Display.InternalResolution", "Internal render"},
                                                                      {"Render.Upscaler.Provider", "Requested upscaler"},
                                                                      {"Render.Upscaler.EffectiveProvider", "Effective upscaler"},
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
    _fps = BistroPlot::Choice("Chart units", "Frame interval (ms)", "FPS", _fps);
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

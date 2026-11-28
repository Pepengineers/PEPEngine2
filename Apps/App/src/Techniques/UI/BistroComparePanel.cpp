#include "BistroComparePanel.h"
#include "BistroReportValues.h"
#include "BistroReportCsvReader.h"
#include "BistroIniMigration.h"
#include "BistroPlot.h"
#include "BistroBenchmarkLaunch.h"
#include "BistroPlacementSearch.h"
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
#include <cstdio>
#include <fstream>
#include <optional>
#include <tuple>

namespace fs = std::filesystem;
namespace BistroReport
{

std::optional<double> Number(const std::string &s)
{
    double v;
    auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    if (r.ec != std::errc{} || r.ptr != s.data() + s.size() || !std::isfinite(v)) return {};
    return v;
}

// Compare archived and current reports using the same setting names at every entry point.
std::string ReportSettingName(const std::string& name)
{
    if (name == "OutputResolution") return "Render.Display.OutputResolution";
    if (name == "RenderResolution") return "Render.Display.InternalResolution";
    if (name == "EffectiveUpscaler") return "Render.Upscaler.EffectiveProvider";
    if (name == "EffectiveFrameGeneration") return "Render.FrameGeneration.EffectiveProvider";
    if (name.rfind("App.", 0) != 0 && name.rfind("Render.", 0) != 0) return name;
    auto key = fs::u8path(name).wstring();
    MigrateBistroIniKey(key);
    return key.empty() ? name : fs::path(key).u8string();
}

void ReadSetting(Settings &settings, const std::string &field, const std::string &value)
{
    const auto name = ReportSettingName(field);
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
    if (value == "local") return "Local";
    if (value == "true") return "On";
    if (value == "false") return "Off";
    auto name = key == "Render.Upscaler.EffectiveProvider" ? "Render.Upscaler.Provider" :
        key == "Render.FrameGeneration.EffectiveProvider" ? "Render.FrameGeneration.Provider" : key;
    IConsoleVariable* variable{};
    int id{};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), id);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) return value;
    const auto leaf = std::string_view(key).substr(key.rfind('.') + 1);
    if ((leaf == "Adapter" || leaf == "EffectiveOwner" || leaf == "RequestedOwner") && id >= -1)
        return id == -1 ? "Scene GPU" : "GPU [" + value + "]";
    if (leaf == "EffectiveSync" || leaf == "RequestedSync")
    {
        const auto adapter = key.rfind(".Adapter.");
        if (adapter != key.npos) return DisplayValue(key.substr(0, adapter) + ".CrossSync", value);
    }
    if (key == "Render.ReflectionProbes.UpdateFrequency")
    {
        constexpr const char* budgets[]{"Snapshot only", "One face", "One probe", "Half the probes", "All probes"};
        if (id >= 0 && size_t(id) < std::size(budgets)) return budgets[id];
    }
    if (BenchmarkEngine::GetLocator().GetModule<ConsoleModule>()->TryFindConsoleVariable(fs::u8path(name).wstring(), variable))
    {
        if (variable->GetType() == EConsoleVariableType::Boolean && (id == 0 || id == 1))
            return id ? "On" : "Off";
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
    if (key == "Render.ReflectionProbes.UpdateFrequency") return "Probe update budget";
    if (key == "Render.ReflectionProbes.ShowSpheres") return "Probe debug spheres";
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

Change SettingChange(const std::string& key, const std::string& before, const std::string& after,
                     const std::string& beforeLabel, const std::string& afterLabel)
{
    if (before.empty() || after.empty() || before == after) return Change::None;
    if (const auto a = SettingNumber(before); a && a == SettingNumber(after)) return Change::None;
    if ((beforeLabel == "On" || beforeLabel == "Off") && (afterLabel == "On" || afterLabel == "Off"))
    {
        return afterLabel == "On" ? Change::Enabled : Change::Disabled;
    }
    const auto direction = [](double oldValue, double newValue) {
        if (oldValue == newValue) return Change::Switched;
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
    if (leaf == "EffectiveOwner" || leaf == "ObservedOwner") return Change::Moved;
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
    const auto endsWith = [&](std::string_view suffix) {
        return leaf.size() >= suffix.size() && leaf.substr(leaf.size() - suffix.size()) == suffix;
    };
    if (leaf == "Resolution" || leaf == "Width" || leaf == "Height" || endsWith("Count") ||
        endsWith("Samples") || endsWith("Seconds") || key == "Render.ReflectionProbes.UpdateFrequency")
    {
        const auto a = Number(before), b = Number(after);
        if (a && b) return direction(*a, *b);
    }
    return Change::Switched;
}

const char* ChangeLabel(Change change)
{
    constexpr const char* names[]{"", "Raised", "Reduced", "Enabled", "Disabled", "Moved", "Switched"};
    return names[size_t(change)];
}

ComparedField CompareField(const std::string& key, const std::string& before, const std::string& after,
    FieldKind kind, bool bothRecorded)
{
    ComparedField row{key, kind == FieldKind::Technique ? key : SettingLabel(key), {}, {}, Change::None};
    if (kind == FieldKind::Technique)
        for (const auto& entry : BistroPlacement::Techniques)
            if (key == entry.Pass && entry.Quality) { row.Key = fs::path(entry.Quality).u8string(); break; }
    const auto display = [&](const std::string& raw) -> std::string {
        if (raw.empty()) return kind == FieldKind::Technique && bothRecorded ? "Inactive" : "";
        return kind == FieldKind::Metric ? raw : DisplayValue(row.Key, raw);
    };
    row.Before = display(before);
    row.After = display(after);
    if (kind != FieldKind::Metric)
        row.Delta = SettingChange(row.Key, before, after, row.Before, row.After);
    if (kind == FieldKind::Placement && row.Delta != Change::None &&
        (key.substr(key.rfind('.') + 1) == "Adapter" || (before.front() == '[' && after.front() == '[')))
        row.Delta = Change::Moved;
    if (kind == FieldKind::Technique && bothRecorded && (before.empty() || after.empty()))
        row.Delta = before.empty() ? Change::Enabled : Change::Disabled;
    return row;
}
}

using namespace BistroReport;

namespace
{
// Keep the middle column in the same place for preset, placement and history comparisons.
constexpr std::array<ImU32, 2> ComparisonColors{IM_COL32(102, 219, 178, 255), IM_COL32(248, 195, 110, 255)};

void CenterInCell(const char* text)
{
    const float spare = ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(text).x;
    if (spare > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + spare * .5f);
}

bool BeginComparisonTable(const char* id, const char* left, const char* middle, const char* right, bool historyColors = false)
{
    if (!ImGui::BeginTable(id, left ? 3 : 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
        ImGuiTableFlags_SizingStretchProp)) return false;
    if (left) ImGui::TableSetupColumn(left, ImGuiTableColumnFlags_WidthStretch, 1.f);
    ImGui::TableSetupColumn(middle, ImGuiTableColumnFlags_WidthStretch, 1.15f);
    ImGui::TableSetupColumn(right, ImGuiTableColumnFlags_WidthStretch, 1.25f);
    ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
    if (left)
    {
        ImGui::TableNextColumn();
        if (historyColors) ImGui::PushStyleColor(ImGuiCol_Text, ComparisonColors[0]);
        ImGui::TextWrapped("%s", left);
        if (historyColors) ImGui::PopStyleColor();
    }
    ImGui::TableNextColumn();
    if (left) CenterInCell(middle);
    ImGui::TextWrapped("%s", middle);
    ImGui::TableNextColumn();
    if (historyColors) ImGui::PushStyleColor(ImGuiCol_Text, ComparisonColors[1]);
    ImGui::TextWrapped("%s", right);
    if (historyColors) ImGui::PopStyleColor();
    return true;
}

void ComparisonRow(const std::string& label, const std::string& before, const std::string& after,
                   Change change = Change::None, const char* hint = nullptr)
{
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    if (ImGui::TableGetColumnCount() == 3)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{.73f, .78f, .82f, 1.f});
        ImGui::TextWrapped("%s", before.empty() ? "Not recorded" : before.c_str());
        ImGui::PopStyleColor();
        ImGui::TableNextColumn();
        ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, IM_COL32(100, 133, 147, 27));
        CenterInCell(label.c_str());
    }
    ImGui::TextWrapped("%s", label.c_str());
    if (hint && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", hint);
    ImGui::TableNextColumn();
    ImGui::TextWrapped("%s", after.empty() ? "Not recorded" : after.c_str());
    if (change == Change::None) return;

    struct Marker { ImWchar Glyph; const char* Arrow; };
    static constexpr Marker markers[]{
        {0, ""}, {0x2191, "\xE2\x86\x91"}, {0x2193, "\xE2\x86\x93"}, {0, ""}, {0, ""},
        {0x2192, "\xE2\x86\x92"}, {0x2194, "\xE2\x86\x94"}};
    const auto& marker = markers[size_t(change)];
    const char* arrow = marker.Glyph && ImGui::GetFont()->IsGlyphInFont(marker.Glyph) ? marker.Arrow : "";
    const auto color = change == Change::Higher ? ImVec4{.44f,.88f,.77f,1.f} :
        change == Change::Lower ? ImVec4{1.f,.78f,.47f,1.f} : ImVec4{.66f,.81f,.92f,1.f};
    BistroPlot::ContinueRowIfFits(ImGui::CalcTextSize(arrow).x + ImGui::CalcTextSize(ChangeLabel(change)).x + ImGui::GetFontSize() * .3f);
    ImGui::TextColored(color, "%s%s%s", arrow, *arrow ? " " : "", ChangeLabel(change));
}

bool BeginTimingTable(const char* id, const char* left, const char* right)
{
    if (!ImGui::BeginTable(id, 4, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
        return false;
    const char* labels[]{"Metric", left, right, "Delta (B - A)"};
    for (int i = 0; i < 4; ++i)
        ImGui::TableSetupColumn(labels[i], ImGuiTableColumnFlags_WidthStretch, i == 0 ? 1.4f : 1.f);
    ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
    for (int i = 0; i < 4; ++i)
    {
        ImGui::TableNextColumn();
        if (i == 1 || i == 2) ImGui::PushStyleColor(ImGuiCol_Text, ComparisonColors[i-1]);
        ImGui::TextWrapped("%s", labels[i]);
        if (i == 1 || i == 2) ImGui::PopStyleColor();
    }
    return true;
}

void MetricComparisonRow(const char* label, double before, double after, bool comparable, bool fps = false)
{
    const char* unit = fps ? "FPS" : "ms";
    char left[48]{}, right[48]{};
    if (before > 0) std::snprintf(left, sizeof(left), "%.2f %s", before, unit);
    if (after > 0) std::snprintf(right, sizeof(right), "%.2f %s", after, unit);
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextWrapped("%s", label);
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(*left ? left : "--");
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(*right ? right : "--");
    ImGui::TableNextColumn();
    if (!comparable || before <= 0 || after <= 0)
    {
        ImGui::TextDisabled("--");
        return;
    }

    const double difference = after - before;
    char delta[96];
    std::snprintf(delta, sizeof(delta), "%+.2f %s", difference, unit);
    const bool improved = fps ? difference > 0 : difference < 0;
    const auto color = std::abs(difference) < .005 ? ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled) :
        improved ? ImVec4{.44f,.88f,.77f,1} : ImVec4{1.f,.78f,.47f,1};
    ImGui::TextColored(color, "%s", delta);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%+.1f%% from the reference", 100. * difference / before);
}

bool ShowAllSettings(const char* title, bool canCompare, bool defaultAll = false)
{
    ImGui::Separator();
    const float available = ImGui::GetContentRegionAvail().x;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(title);
    if (!canCompare) return true;
    const auto id = ImGui::GetID("all");
    auto* storage = ImGui::GetStateStorage();
    const bool all = storage->GetBool(id, defaultAll);
    const float filterWidth = ImGui::CalcTextSize("Changes only").x + ImGui::CalcTextSize("All settings").x +
        4.f * ImGui::GetStyle().FramePadding.x + 2.f;
    const float gap = available - ImGui::GetItemRectSize().x - filterWidth;
    if (gap >= ImGui::GetStyle().ItemSpacing.x) ImGui::SameLine(0.f, gap);
    const bool selected = BistroPlot::Choice("", "Changes only", "All settings", all);
    if (all != selected) storage->SetBool(id, selected);
    return selected;
}

void DrawSettings(const Settings& values, const Settings& before, const char* title = "Pass settings",
                  const char* resultColumn = "Measured pass", const char* leftColumn = "Before run",
                  bool compareRuns = false, bool defaultAll = false)
{
    ImGui::PushID(title);
    const bool compare = compareRuns || !before.empty();
    const bool all = ShowAllSettings(title, compare, defaultAll);
    if (!compare) ImGui::TextDisabled("Pre-run settings unavailable.");
    std::map<std::string, std::vector<const std::string*>> groups;
    const auto custom = std::to_string(int(BistroRender::CloudPictureQuality::Custom));
    const auto add = [&](const std::string& key) {
        if (!compareRuns && (key == "RouteSeconds" || key == "WarmupSeconds" || key == "PaintingHoldSeconds")) return;
        if (key == "Render.CloudPicture.Width" || key == "Render.CloudPicture.Height")
        {
            const auto& quality = Value(values, "Render.CloudPicture.Quality");
            if (!quality.empty() && quality != custom && !(compare && Value(before, "Render.CloudPicture.Quality") == custom)) return;
        }
        if (!all)
        {
            const auto& a = Value(before, key);
            const auto& b = Value(values, key);
            if (a.empty() || b.empty() || a == b) return;
            if (const auto n = SettingNumber(a); n && n == SettingNumber(b)) return;
        }
        groups[SettingGroup(key)].push_back(&key);
    };
    for (const auto& [key, value] : values) add(key);
    if (compareRuns && all)
        for (const auto& [key, value] : before) if (!values.count(key)) add(key);
    if (groups.empty()) ImGui::TextDisabled("%s", all ? "No settings recorded." : "No recorded differences.");
    else if (BeginComparisonTable(all ? "All" : "Changes", compare ? leftColumn : nullptr, "Setting", resultColumn, compareRuns))
    {
        for (const auto& [group, rows] : groups)
        {
            const auto title = SettingLabel(group);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(46, 62, 72, 255));
            if (!ImGui::TreeNodeEx(title.c_str(), ImGuiTreeNodeFlags_NoTreePushOnOpen |
                ImGuiTreeNodeFlags_SpanAllColumns | (!all || group == "Display" ? ImGuiTreeNodeFlags_DefaultOpen : 0),
                "%s (%zu)", title.c_str(), rows.size())) continue;
            for (const std::string* name : rows)
            {
                const auto& key = *name;
                const auto& a = Value(before, key);
                const auto& b = Value(values, key);
                auto old = a.empty() ? "" : DisplayValue(key, a);
                auto selected = b.empty() ? "" : DisplayValue(key, b);
                const auto change = SettingChange(key, a, b, old, selected);
                if (key.size() >= 10 && key.compare(key.size() - 10, 10, ".CrossSync") == 0)
                {
                    const auto adapter = key.substr(0, key.size() - 10) + ".Adapter";
                    if (!old.empty() && Value(before, adapter) == "-1") old += " (unused locally)";
                    if (!selected.empty() && Value(values, adapter) == "-1") selected += " (unused locally)";
                }
                ComparisonRow(SettingLabel(key), old, selected, change, key.c_str());
            }
        }
        ImGui::EndTable();
    }
    ImGui::PopID();
}

std::string RunMode(const std::string &mode)
{
    if (mode == "manual_placement" || mode == "manual_run") return "Manual";
    if (mode == "multi_gpu_profiles") return "MGPU";
    if (mode == "single_gpu_profiles") return "Single GPU";
    if (mode.find("search") != mode.npos) return "Async";
    return "Unknown";
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
        if (it.depth() == 0 && name == "Exports")
        {
            it.disable_recursion_pending();
            continue;
        }
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
        Values adapterLabels;
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
            // Older inline summary rows contain CPU scope P95. Only a pass row records present-interval P95.
            FrameTiming timing;
            if (csv.Get("type") == "pass")
                timing = {Number(csv.Get("fps")).value_or(-1.), Number(csv.Get("p95_ms")).value_or(-1.)};
            run.Passes.push_back({std::move(path), std::move(label), csv.Get("mask"), std::move(range), offset, timing});
            if (run.Mode == "Manual") run.SummaryFps = Number(csv.Get("fps"));
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
                    if (value == "manual_search") run.DisplayMode = "Manual queue search";
                    if (value == "automatic_search") run.DisplayMode = "Automatic queue search";
                    if (value == "exhaustive_search") run.DisplayMode = "Exhaustive queue search";
                }
                else if (key == "Profile")
                    run.Profile = value;
                else if (key == "MapId") run.MapId = value;
                else if (key == "RequestedOutput") run.Output = value;
                else if (key == "PrimaryAdapter") adapterLabels["-1"] = value;
                else if (key == "EligiblePeer")
                {
                    // The report labels each GPU as "[DXGI hardware index] model".
                    const auto end = value.find(']');
                    if (!value.empty() && value.front() == '[' && end != value.npos)
                        adapterLabels[value.substr(1, end - 1)] = value;
                }
                else if (key == "BaseBudgetSeconds") run.BaseBudget = Number(value);
                else if (key == "AddedFullRouteSeconds") run.AddedRoutes = Number(value);
                else if (key == "BudgetSeconds") run.Budget = Number(value);
                else if (key == "ElapsedSeconds") run.Elapsed = Number(value);
                else if (key.size() > 14 && key.rfind("SearchPhase.", 0) == 0 && key[12] >= '0' && key[12] <= '3' && key[13] == '.')
                {
                    auto& budget = run.Phases[key[12] - '0'];
                    const auto field = key.substr(14);
                    if (field == "BaseSeconds") budget.Base = Number(value);
                    else if (field == "AddedFullRouteSeconds") budget.Added = Number(value);
                    else if (field == "BudgetSeconds") budget.Total = Number(value);
                }
                else if (key.rfind("ReconstructionComparison.", 0) == 0)
                {
                    constexpr auto start = std::string_view("ReconstructionComparison.").size();
                    const auto dot = key.find('.', start);
                    if (dot == key.npos) continue;
                    const auto id = Number(key.substr(start, dot - start));
                    if (!id || *id < 1 || *id > UINT32_MAX || *id != std::floor(*id)) continue;
                    auto& group = run.Reconstructions[unsigned(*id)];
                    const auto name = key.substr(dot + 1);
                    if (name == "Mode") group.Mode = value;
                    else if (name == "Status") group.Status = value;
                    else if (name == "Participant")
                    {
                        const auto split = value.find(' ');
                        const auto trial = value.substr(0, split);
                        group.Participants.push_back(trial);
                        if (split != value.npos) run.ReconstructionTrials[trial]["Render.Upscaler.Provider"] = value.substr(split + 1);
                    }
                    else if (name == "WinningTrial") group.Winner = value;
                }
                else if (key == "MinimumFps") run.MinimumFps = Number(value);
                else if (key == "RouteId") run.RouteId = value;
                else if (key == "FinalStatus") run.FinalStatus = value;
                else if (key == "SelectedImage") run.SelectedImage = value;
                else if (key == "StopReason") run.StopReason = value;
                else if (key == "FastestMask")
                {
                    const auto mask = Number(value);
                    if (mask && *mask >= 0 && *mask <= UINT32_MAX && *mask == std::floor(*mask))
                        run.FinalMask = std::to_string(uint32_t(*mask));
                }
                else if (key.rfind("Before.", 0) == 0) run.Before[ReportSettingName(key.substr(7))] = value;
                else if (key.rfind("MaskBit", 0) == 0) run.MaskBits[key.substr(7)] = ReportSettingName(value);
                else if (key.rfind("Trial.", 0) == 0)
                {
                    auto dot = key.find('.', 6);
                    if (dot == key.npos) continue;
                    const auto id = key.substr(6, dot - 6);
                    const auto field = key.substr(dot + 1);
                    if (field == "LastPassCSV") run.TrialPasses[id] = value;
                    if (field == "ReconstructionComparison") run.ReconstructionTrials.try_emplace(id);
                    if (const auto trial = run.ReconstructionTrials.find(id); trial != run.ReconstructionTrials.end())
                        trial->second[field] = value;
                }
                else if (key.rfind("Native.", 0) == 0 || key.rfind("Upscaled.", 0) == 0)
                {
                    const bool upscaled = key.front() == 'U';
                    const auto prefix = (upscaled ? "Upscaled." : "Native.") + run.Profile + ".";
                    if (key.rfind(prefix, 0) != 0) continue;
                    const auto field = key.substr(prefix.size());
                    auto& winner = run.Winners[upscaled];
                    const bool splitPlacement = field.rfind("SingleGPU.", 0) == 0 || field.rfind("MultiGPU.", 0) == 0;
                    auto& placement = splitPlacement ? winner.Placements[field.front() == 'M'] : static_cast<PlacementResult&>(winner);
                    const auto key = splitPlacement ? field.substr(field.find('.') + 1) : field;
                    if (!splitPlacement && key == "ImprovedSetting") winner.Improvements.push_back(value);
                    else if (key == "Technique")
                    {
                        const auto split = value.find('=');
                        if (split != value.npos) placement.Techniques[value.substr(0, split)] = value.substr(split + 1);
                    }
                    else if (const auto dot = key.rfind('.'); dot != key.npos && key.compare(dot, key.npos, ".EffectiveOwner") == 0)
                    {
                        const auto& label = Value(adapterLabels, value);
                        placement.GpuSettings[ReportSettingName(key.substr(0, dot))] = label.empty() ? value : label;
                    }
                    else if (dot != key.npos && key.compare(dot, key.npos, ".Effective") == 0)
                        placement.GpuSettings[ReportSettingName(key.substr(0, dot))] = value;
                    else if (!splitPlacement && key.rfind("Preset.", 0) == 0) winner.Settings[ReportSettingName(key.substr(7))] = value;
                    else if (key.find('.') == key.npos) placement.Fields[key] = value;
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
        if (run.Passes.empty() && run.Winners[0].Fields.empty() && run.Winners[1].Fields.empty() && run.FinalStatus.empty()) continue;
        if (run.DisplayMode.empty()) run.DisplayMode = run.Profile.empty() ? run.Mode : run.Profile + " search";
        // Older searches changed resolution between trials. Show the chosen image's output
        // only when the winner is identifiable in this report.
        const auto& native = run.Winners[0].Fields;
        const auto& upscaled = run.Winners[1].Fields;
        const Settings* selected = nullptr;
        if (run.SelectedImage == "Native" || (run.SelectedImage.empty() && Value(upscaled, "Trial").empty()))
            selected = &native;
        else if (run.SelectedImage == "Upscaled" || (run.SelectedImage.empty() && Value(native, "Trial").empty()))
            selected = &upscaled;
        if (selected && !Value(*selected, "Trial").empty())
        {
            run.SummaryFps = Number(Value(*selected, "FPS"));
            if (run.Output.empty()) run.Output = Value(*selected, "Output");
        }
        run.Timestamp = file.stem().string();
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
                run.Timestamp = date;
        }
        run.Adapters = std::move(adapterLabels);
        run.MapName = "Archived run";
        if (!run.MapId.empty())
        {
            const auto found = std::find_if(std::begin(BistroRun::Maps), std::end(BistroRun::Maps),
                [&](const auto& map) { return run.MapId == map.Id; });
            run.MapName = found == std::end(BistroRun::Maps) ? run.MapId : found->Name;
        }
        if (run.Output.empty()) run.Output = Value(run.Settings, "Render.Display.OutputResolution");
        run.Label = run.MapName + " / " + run.DisplayMode + "  |  " + run.Timestamp;
        if (!run.FinalStatus.empty()) run.Label += "  |  " + run.FinalStatus;
        _runs.push_back(std::move(run));
    }
    if (ec && ec != std::errc::no_such_file_or_directory) _notice = ec.message();
    std::sort(_runs.begin(), _runs.end(),
              [](const Run &a, const Run &b) { return a.Path.filename() > b.Path.filename(); });
    _filterOptions = {};
    for (const auto& run : _runs)
    {
        _filterOptions[0].push_back(run.MapName);
        _filterOptions[1].push_back(run.DisplayMode);
        if (!run.Output.empty()) _filterOptions[2].push_back(run.Output);
    }
    for (auto& options : _filterOptions)
    {
        std::sort(options.begin(), options.end());
        options.erase(std::unique(options.begin(), options.end()), options.end());
    }
    // A saved reference can link to a trial that reused the full route of another profile.
    std::map<fs::path, const Run*> reports;
    std::map<fs::path, std::pair<int, int>> fullPasses;
    for (int r = 0; r < int(_runs.size()); ++r)
    {
        const auto& run = _runs[r];
        reports.emplace(run.Path, &run);
        for (int p = 0; p < int(run.Passes.size()); ++p)
            if (run.Passes[p].Range.Section == "Full route") fullPasses.emplace(run.Passes[p].Path, std::pair{r, p});
    }
    for (auto& run : _runs)
        for (auto& winner : run.Winners)
        {
            if (winner.Confirmed())
            {
                winner.Settings["Render.Display.OutputResolution"] = Value(winner.Fields, "Output");
                winner.Settings["Render.Display.InternalResolution"] = Value(winner.Fields, "Render");
                winner.Settings["Render.Upscaler.EffectiveProvider"] = Value(winner.Fields, "Provider");
            }
            for (auto* result : {static_cast<PlacementResult*>(&winner), &winner.Placements[0], &winner.Placements[1]})
            {
                result->RunIndex = result->PassIndex = -1;
                if (!result->Confirmed()) continue;
                const auto& source = Value(result->Fields, "CSV");
                const auto path = source.empty() ? run.Path : (run.Path.parent_path() / fs::u8path(source)).lexically_normal();
                const auto report = reports.find(path);
                if (report == reports.end()) continue;
                const auto& ref = Value(report->second->TrialPasses, Value(result->Fields, "Trial"));
                if (ref.empty()) continue;
                const auto pass = fullPasses.find((path.parent_path() / fs::u8path(ref)).lexically_normal());
                if (pass != fullPasses.end()) std::tie(result->RunIndex, result->PassIndex) = pass->second;
            }
        }
    _placementPasses = {};
    _lastPass = {};
    _openError.clear();
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
    if (!r.MapId.empty()) s.Settings["MapId"] = r.MapId;
    if (!r.RouteId.empty()) s.Settings["RouteId"] = r.RouteId;
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
            if (key.rfind("Trial.", 0) != 0 || dot == key.npos) continue;
            const auto name = key.substr(dot + 1);
            const auto& value = config.Get("value");
            ReadSetting(s.Settings, name, value);
            if (s.Range.Section != "Full route") continue;
            if (name == "ActiveTechniqueLevel")
            {
                const auto split = value.find('=');
                if (split != value.npos) s.Observed.Techniques[value.substr(0, split)] = value.substr(split + 1);
            }
            else if ((name.size() >= 7 && name.compare(name.size() - 7, 7, "Adapter") == 0) ||
                name == "Skinning" || name.rfind("Probe ", 0) == 0 || name.rfind("Local shadow page ", 0) == 0 ||
                name.rfind("CSM ", 0) == 0)
            {
                // The route may visit the same technique on several physical adapters.
                if (value.empty() || value.front() != '[') continue;
                auto& owners = s.Observed.GpuSettings[name];
                if (!owners.empty()) owners += ", ";
                owners += value;
            }
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
    s.Timing.Fps = 1000 * intervals.size() / sum;
    const auto p95 = intervals.begin() + (intervals.size() * 95 - 1) / 100;
    std::nth_element(intervals.begin(), p95, intervals.end());
    s.Timing.P95 = *p95;
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

namespace
{
void DrawSeriesLabel(size_t index, const char* label)
{
    const float em = ImGui::GetFontSize();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float y = origin.y + ImGui::GetTextLineHeight() * .5f;
    const auto color = ComparisonColors[index];
    auto* draw = ImGui::GetWindowDrawList();
    if (index == 1)
    {
        draw->AddLine({origin.x, y}, {origin.x + .65f * em, y}, color, .16f * em);
        draw->AddLine({origin.x + em, y}, {origin.x + 1.65f * em, y}, color, .16f * em);
    }
    else
        draw->AddLine({origin.x, y}, {origin.x + 1.65f * em, y}, color, .16f * em);
    ImGui::Dummy({1.65f * em, ImGui::GetTextLineHeight()});
    ImGui::SameLine();
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(color), "%c   %s", index ? 'B' : 'A', label);
}
}

void BistroComparePanel::DrawSelection(Selection &s, size_t index)
{
    ImGui::PushID(int(index));
    DrawSeriesLabel(index, index ? "Comparison" : "Reference");
    if (!s.Samples.empty() && s.Error.empty())
    {
        BistroPlot::ContinueRowIfFits(ImGui::CalcTextSize("Details").x + 2.f * ImGui::GetStyle().FramePadding.x);
        if (ImGui::SmallButton("Details")) ImGui::OpenPopup("Pass details");
    }
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
        if (ImGui::BeginPopup("Pass details"))
        {
            const auto& output = Value(s.Settings, "Render.Display.OutputResolution");
            const auto& provider = Value(s.Settings, "Render.Upscaler.EffectiveProvider");
            if (!output.empty() || !provider.empty())
                ImGui::Text("Output %s   |   Upscaler %s", output.empty() ? "--" : output.c_str(),
                    DisplayValue("Render.Upscaler.EffectiveProvider", provider).c_str());
            if (s.Range.Begin >= 0 && s.Range.End >= s.Range.Begin)
                ImGui::TextDisabled("%.2f - %.2f s  |  %s", s.Range.Begin, s.Range.End,
                    s.AfterRender ? "after Render / Present" : "before Present");
            for (int i = 0; i < 2; ++i)
            {
                const auto& winner = _runs[r].Winners[i];
                if (winner.RunIndex == r && winner.PassIndex == s.PassIndex && !Value(winner.Fields, "Trial").empty())
                    ImGui::Text("%s  |  %s", i ? "Upscaled" : "Native", Value(winner.Fields, "Status").c_str());
            }
            ImGui::EndPopup();
        }
    }
    if (!s.Error.empty()) ImGui::TextWrapped("%s", s.Error.c_str());
    ImGui::PopID();
}

BistroComparePanel::PlotLines BistroComparePanel::RunLines(const Winner* placement, const Selection& current,
    std::array<Selection, 2>& cache)
{
    PlotLines lines{};
    if (!placement || (placement->Placements[0].Fields.empty() && placement->Placements[1].Fields.empty()))
    {
        if (!current.Samples.empty() && current.Error.empty()) lines[0].Data = &current;
        return lines;
    }
    for (size_t i = 0; i < lines.size(); ++i)
    {
        const auto& result = placement->Placements[i];
        auto& line = lines[i];
        line.Label = i == 0 ? "Single GPU" : "Multi-GPU";
        if (!result.Confirmed())
        {
            line.Label += " / no confirmed full-route pass";
            continue;
        }
        const Selection* data = &current;
        if (current.RunIndex != result.RunIndex || current.PassIndex != result.PassIndex)
        {
            auto& saved = cache[i];
            if (saved.RunIndex != result.RunIndex || saved.PassIndex != result.PassIndex)
                Select(saved, result.RunIndex, result.PassIndex);
            data = &saved;
        }
        if (data->Samples.empty() || !data->Error.empty())
            line.Label += " / " + (data->Error.empty() ? std::string("frame trace unavailable") : data->Error);
        else
        {
            line.Data = data;
            char fps[40];
            std::snprintf(fps, sizeof(fps), " / %.1f FPS", data->Timing.Fps);
            line.Label += fps;
        }
    }
    return lines;
}

void BistroComparePanel::DrawPlot(const PlotLines& lines, bool fps, double target)
{
    for (size_t i = 0; i < lines.size(); ++i)
    {
        const auto& line = lines[i];
        if (line.Label.empty()) continue;
        if (!line.Data) { ImGui::TextDisabled("%s", line.Label.c_str()); continue; }
        BistroPlot::ContinueRowIfFits(5.f * ImGui::GetFontSize() + ImGui::CalcTextSize(line.Label.c_str()).x);
        DrawSeriesLabel(i, line.Label.c_str());
    }
    if (!lines[0].Data && !lines[1].Data) return;
    const auto& a = *(lines[0].Data ? lines[0].Data : lines[1].Data);
    const auto& b = lines[1].Data ? *lines[1].Data : a;
    const double begin = std::min(a.Samples.front().RouteSeconds, b.Samples.front().RouteSeconds);
    const double end = std::max(a.Samples.back().RouteSeconds, b.Samples.back().RouteSeconds);
    const double span = std::max(.001, end - begin);
    const double peak = std::max(target > 0 ? (fps ? target : 1000. / target) : 0., fps ? std::max(a.MaxFps, b.MaxFps) : std::max(a.MaxMs, b.MaxMs));
    const auto format = [](double value, const char *fmt) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), fmt, value);
        return std::string(buf);
    };
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float em = ImGui::GetFontSize();
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const float yLabelWidth = ImGui::CalcTextSize(format(peak, "%.1f").c_str()).x;
    const float timeLabelWidth = ImGui::CalcTextSize(format(end, "%.1fs").c_str()).x;
    const float left = std::max(yLabelWidth + .75f * em, .5f * timeLabelWidth);
    const float width = std::max(1.f, available.x - left - .5f * timeLabelWidth);
    // Keep the three A/B metrics visible below the plot at typical window sizes.
    const float height = lines[0].Data && lines[1].Data ? std::clamp(available.y - 10.f * em, 5.5f * em, 19.f * em)
                                    : std::clamp(available.y * .65f, 7.f * em, 19.f * em);
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
        draw->AddLine({top.x + width * f, top.y}, {top.x + width * f, top.y + height}, ImGui::GetColorU32(ImGuiCol_Border));
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
    // Draw B's dashes over A so identical traces still expose both series.
    for (size_t iSeries = 0; iSeries < lines.size(); ++iSeries)
    {
        if (!lines[iSeries].Data) continue;
        const auto& s = *lines[iSeries].Data;
        const auto color = ComparisonColors[iSeries];
        ImVec2 previous = point(s.Samples[s.Trace.front()]);
        draw->AddCircleFilled(previous, .15f * em, color);
        float phase = 0;
        for (size_t i = 1; i < s.Trace.size(); ++i)
        {
            const auto p = point(s.Samples[s.Trace[i]]);
            if (iSeries == 1)
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
        for (size_t iSeries = 0; iSeries < lines.size(); ++iSeries)
        {
            if (!lines[iSeries].Data) continue;
            const auto& s = *lines[iSeries].Data;
            const char id = iSeries == 0 ? 'A' : 'B';
            if (t < s.Samples.front().RouteSeconds || t > s.Samples.back().RouteSeconds)
            {
                ImGui::Text("%c   --", id);
                continue;
            }
            const auto& sample = s.Samples[BistroPlot::NearestFrame(s.Samples, t)];
            const auto pos = point(sample);
            const auto color = ComparisonColors[iSeries];
            draw->AddCircleFilled(pos, .3f * em, color);
            draw->AddCircle(pos, .3f * em, muted);
            ImGui::Separator();
            if (!lines[iSeries].Label.empty()) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(color), "%c   %s", id, lines[iSeries].Label.c_str());
            BistroPlot::DrawFrameDetails(sample.Number, sample.RouteSeconds, sample.Section.c_str(), sample.FrameMs,
                                         sample.IntervalMs);
        }
        ImGui::EndTooltip();
    }
    ImGui::Dummy({0, 1.4f * em});
}

void BistroComparePanel::UpdateHistory(const fs::path& latestCompleted, bool force)
{
    if (force || !_indexed || latestCompleted != _latestCompleted)
    {
        Refresh(force);
        _indexed = true;
        if (!latestCompleted.empty() && latestCompleted != _latestCompleted)
        {
            _historyPath = latestCompleted;
            _showComparison = false;
            _filters = {};
        }
        _latestCompleted = latestCompleted;
    }
}

void BistroComparePanel::DrawComparedTimings(const FrameTiming& a, const FrameTiming& b, const char* left,
    const char* right, bool comparable)
{
    if (BeginTimingTable("Frame timings", left, right))
    {
        MetricComparisonRow("Mean frame time", a.MeanMs(), b.MeanMs(), comparable);
        MetricComparisonRow("Average FPS", a.Fps, b.Fps, comparable, true);
        MetricComparisonRow("P95 frame time", a.P95, b.P95, comparable);
        ImGui::EndTable();
    }
}

void BistroComparePanel::DrawRouteMetrics(const FrameTiming& timing, ImFont* headingFont)
{
    const float em = ImGui::GetFontSize();
    const int columns = ImGui::GetContentRegionAvail().x >= 25.f * em ? 3 : 1;
    if (!ImGui::BeginTable("Route metrics", columns, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoPadOuterX)) return;
    const auto metric = [&](const char* label, double number, const char* unit)
    {
        char value[32] = "--";
        if (number > 0) std::snprintf(value, sizeof(value), "%.1f", number);
        ImGui::TableNextColumn();
        const float height = 4.65f * em + 2.f * ImGui::GetStyle().WindowPadding.y;
        if (ImGui::BeginChild(label, {0, height}, ImGuiChildFlags_Borders,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
        {
            ImGui::TextWrapped("%s", label);
            const float width = ImGui::GetContentRegionAvail().x;
            const float unitWidth = *unit ? ImGui::CalcTextSize(unit).x + .22f * em : 0.f;
            ImGui::PushFont(headingFont, 0.f);
            const float scale = std::clamp((width - unitWidth) / std::max(1.f, ImGui::CalcTextSize(value).x), .8f, 2.85f);
            ImGui::PushFont(headingFont, ImGui::GetStyle().FontSizeBase * scale);
            ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_NavCursor), "%s", value);
            ImGui::PopFont();
            ImGui::PopFont();
            if (*unit && number > 0)
            {
                const float bottom = ImGui::GetItemRectMax().y;
                ImGui::SameLine(0.f, .22f * em);
                ImGui::SetCursorScreenPos({ImGui::GetCursorScreenPos().x, bottom - 1.15f * em});
                ImGui::TextDisabled("%s", unit);
            }
        }
        ImGui::EndChild();
    };
    metric("Mean frame time", timing.MeanMs(), "ms");
    metric("Average FPS", timing.Fps, "");
    metric("P95 frame time", timing.P95, "ms");
    ImGui::EndTable();
}

std::string BistroReport::ImageSummary(const Settings& settings)
{
    const auto& output = Value(settings, "Render.Display.OutputResolution");
    const auto& render = Value(settings, "Render.Display.InternalResolution");
    const auto& provider = Value(settings, "Render.Upscaler.EffectiveProvider");
    const auto& quality = Value(settings, "Render.Upscaler.Quality");
    const auto& requested = Value(settings, "Render.Upscaler.Provider");
    std::string summary;
    const auto append = [&](const std::string& part)
    {
        if (!summary.empty()) summary += "   |   ";
        summary += part;
    };
    if (!requested.empty() && !provider.empty() && requested != provider)
    {
        auto mode = DisplayValue("Render.Upscaler.Provider", requested);
        if (requested != "0" && !quality.empty()) mode += " / " + DisplayValue("Render.Upscaler.Quality", quality);
        append("Requested " + mode);
    }
    if (!provider.empty()) append(DisplayValue("Render.Upscaler.EffectiveProvider", provider));
    if (!quality.empty() && !provider.empty() && provider != "0")
        append(DisplayValue("Render.Upscaler.Quality", quality));
    if (!output.empty()) append(output + " output");
    if (!render.empty())
    {
        std::string dimensions = render + " internal";
        unsigned rw{}, rh{}, ow{}, oh{};
        if (sscanf_s(render.c_str(), "%ux%u", &rw, &rh) == 2 && sscanf_s(output.c_str(), "%ux%u", &ow, &oh) == 2 && ow && oh)
        {
            char scale[64];
            std::snprintf(scale, sizeof(scale), " (%.1f%% x %.1f%%)", 100. * rw / ow, 100. * rh / oh);
            dimensions += scale;
        }
        append(dimensions);
    }
    return summary;
}

std::array<std::string, 8> BistroComparePanel::ReconstructionRow(const Run& run, const ReconstructionComparison& group,
    const std::string& id)
{
    const auto it = run.ReconstructionTrials.find(id);
    if (it == run.ReconstructionTrials.end()) return {id, {}, {}, {}, {}, {}, {}, "pending"};
    const auto& fields = it->second;
    const auto device = Value(fields, "EffectivePostDevice");
    const auto requested = Value(fields, "RequestedPostDevice");
    auto status = id == group.Winner ? std::string("Winner") : Value(fields, "Status");
    if (status.empty()) status = "pending";
    if (const auto& eligibility = Value(fields, "ComparisonEligibility"); !eligibility.empty()) status = eligibility;
    if (const auto& fallback = Value(fields, "ReconstructionFallback"); !fallback.empty()) status += " / " + fallback;
    const auto provider = [&](const char* key) {
        const auto& value = Value(fields, key);
        return value.empty() ? std::string{} : DisplayValue("Render.Upscaler.EffectiveProvider", value);
    };
    auto resolution = Value(fields, "Render.Display.InternalResolution");
    if (const auto& scale = Value(fields, "InternalScale"); !scale.empty()) resolution += " (" + scale + ")";
    const auto& applied = Value(fields, "ReconstructionApplied");
    return {id, provider("Render.Upscaler.Provider"),
        applied.empty() ? provider("Render.Upscaler.EffectiveProvider") : applied,
        device.empty() || device == requested ? requested : requested + " -> " + device,
        std::move(resolution), Value(fields, "FPS"), Value(fields, "P95ms"), std::move(status)};
}

void BistroComparePanel::DrawPlacementComparison(const Winner& winner)
{
    const auto& single = winner.Placements[0];
    const auto& multi = winner.Placements[1];
    const auto& explanation = Value(winner.Fields, "Comparison");
    if (multi.Fields.empty() && explanation.empty()) return;
    constexpr const char* left = "Single GPU reference";
    constexpr const char* right = "Multi-GPU candidate";
    ImGui::SeparatorText("Single GPU vs Multi-GPU");
    if (const auto& gpu = Value(winner.Fields, "Winner"); !gpu.empty())
        ImGui::Text("Winning placement   %s", gpu.c_str());
    const auto gain = Number(Value(winner.Fields, "SameImageFpsGainPct"));
    if (gain) ImGui::Text("Same-image FPS change    %+.1f%%", *gain);
    if (!explanation.empty())
        ImGui::TextWrapped("%s", explanation.c_str());
    if (!Number(Value(multi.Fields, "FPS")))
    {
        if (explanation.empty()) ImGui::TextDisabled("No confirmed multi-GPU candidate.");
        return;
    }
    if (!winner.Improvements.empty())
    {
        ImGui::TextUnformatted("Quality gains in the multi-GPU candidate");
        for (const auto& setting : winner.Improvements) ImGui::BulletText("%s", setting.c_str());
    }
    else if (Number(Value(single.Fields, "FPS")))
        ImGui::TextDisabled("No technique quality gains recorded.");

    DrawTechniques(single, multi, "GPU placement and technique changes", left, right);
    if (!ImGui::TreeNode("Placement metrics and configuration")) return;
    const auto timing = [](const PlacementResult& measured) {
        return FrameTiming{Number(Value(measured.Fields, "FPS")).value_or(-1.),
            Number(Value(measured.Fields, "P95ms")).value_or(-1.)};
    };
    DrawComparedTimings(timing(single), timing(multi), "A / Single GPU", "B / Multi-GPU", gain.has_value());
    if (BeginComparisonTable("Placement results", left, "Configuration", right))
    {
        for (const auto& [label, key] : std::initializer_list<std::pair<const char*, const char*>>{
            {"Status", "Status"}, {"Output", "Output"}, {"Internal render", "Render"}})
            ComparisonRow(label, Value(single.Fields, key), Value(multi.Fields, key));
        ComparisonRow("Upscaler", DisplayValue("Render.Upscaler.EffectiveProvider", Value(single.Fields, "Provider")),
            DisplayValue("Render.Upscaler.EffectiveProvider", Value(multi.Fields, "Provider")));
        ImGui::EndTable();
    }

    ImGui::TreePop();
}

void BistroComparePanel::DrawTechniques(const PlacementResult& single, const PlacementResult& multi,
    const char* title, const char* left, const char* right)
{
    const bool bothHaveTechniques = !single.Techniques.empty() && !multi.Techniques.empty();
    const bool bothHaveOwners = !single.GpuSettings.empty() && !multi.GpuSettings.empty();
    ImGui::PushID(title);
    const bool all = ShowAllSettings(title, bothHaveTechniques || bothHaveOwners);
    if (BeginComparisonTable("Placement details", left, "Technique or setting", right))
    {
        unsigned rows = 0;
        const auto group = [&](const Values& a, const Values& b, bool technique) {
            const auto item = [&](const std::string& key, const std::string& before, const std::string& after) {
                const auto row = CompareField(key, before, after, technique ? FieldKind::Technique : FieldKind::Placement,
                    technique ? bothHaveTechniques : bothHaveOwners);
                if (!all && row.Delta == Change::None) return;
                ++rows;
                ComparisonRow(row.Label, row.Before, row.After, row.Delta, row.Key.c_str());
            };
            for (const auto& [key, value] : a) item(key, value, Value(b, key));
            for (const auto& [key, value] : b) if (!a.count(key)) item(key, {}, value);
        };
        group(single.GpuSettings, multi.GpuSettings, false);
        group(single.Techniques, multi.Techniques, true);
        ImGui::EndTable();
        if (!rows) ImGui::TextDisabled("%s", all ? "No placement details recorded." : "No recorded placement or technique changes.");
    }
    ImGui::PopID();
}

bool BistroComparePanel::PlacementResult::Confirmed() const
{
    return !Value(Fields, "Trial").empty() && Number(Value(Fields, "FPS")).has_value();
}

bool BistroComparePanel::Winner::Available() const
{
    return Confirmed() || Number(Value(Placements[0].Fields, "FPS")).has_value() ||
        Number(Value(Placements[1].Fields, "FPS")).has_value();
}

BistroComparePanel::FrameTiming BistroComparePanel::SummaryTiming(const Winner* winner, const Selection& selection) const
{
    if (winner)
        return {Number(Value(winner->Fields, "FPS")).value_or(-1.), Number(Value(winner->Fields, "P95ms")).value_or(-1.)};
    if (!selection.Samples.empty() && selection.Error.empty()) return selection.Timing;
    if (selection.PassIndex >= 0) return _runs[selection.RunIndex].Passes[selection.PassIndex].Timing;
    return {};
}

const char* BistroComparePanel::Run::TargetStatus(double fps, const Winner* winner) const
{
    if (fps >= Target()) return "Met";
    const bool within = MinimumFps ? fps >= *MinimumFps :
        winner && Value(winner->Fields, "Status") == "confirmed within tolerance";
    return within ? "Within tolerance" : "Below target";
}

void BistroComparePanel::DrawRun(int index, fs::path& pendingPreset)
{
    const Run& run = _runs[index];
    const float em = ImGui::GetFontSize();
    ImGui::PushFont(_headingFont, ImGui::GetStyle().FontSizeBase * 1.9f);
    ImGui::TextWrapped("%s", run.MapName.c_str());
    ImGui::PopFont();
    const float exportWidth = ImGui::CalcTextSize("Export ZIP").x + 2.f * ImGui::GetStyle().FramePadding.x;
    const float actionWidth = ImGui::CalcTextSize("Compare passes").x + 2.f * ImGui::GetStyle().FramePadding.x +
        ImGui::GetStyle().ItemSpacing.x + exportWidth;
    const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    if (ImGui::GetItemRectMax().x + em < right - actionWidth)
    {
        ImGui::SameLine();
        ImGui::SetCursorScreenPos({right - actionWidth, ImGui::GetCursorScreenPos().y});
    }
    const bool compare = ImGui::Button("Compare passes");
    BistroPlot::ContinueRowIfFits(exportWidth);
    ImGui::BeginDisabled(_exportJob.valid());
    const bool exportZip = ImGui::Button("Export ZIP");
    ImGui::EndDisabled();
    int measuredRun = index;
    int pass = int(run.Passes.size()) - 1;
    const Winner* winner = nullptr;
    const bool native = run.Winners[0].Available(), upscaled = run.Winners[1].Available();
    if (_imageRun != run.Path)
    {
        _upscaled = run.SelectedImage == "Upscaled";
        _imageRun = run.Path;
    }
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
        if (placement->Confirmed())
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
    else if (!run.Profile.empty())
        ImGui::TextDisabled("%s", pass >= 0 ? "No confirmed preset. Showing the last completed measurement." : "No confirmed preset.");

    auto& selection = _lastPass;
    if (selection.RunIndex != measuredRun || selection.PassIndex != pass)
        Select(selection, measuredRun, pass);

    const double target = run.Target();
    const bool hasTrace = !selection.Samples.empty() && selection.Error.empty();
    if (compare)
    {
        if (hasTrace)
        {
            _selected[0].RunIndex = measuredRun;
            _selected[0].PassIndex = pass;
            _loadComparison = true;
        }
        _showComparison = true;
    }
    const auto& outcome = run.FinalStatus.empty() ? run.StopReason : run.FinalStatus;
    const auto& measuredOutput = winner ? Value(winner->Fields, "Output") : Value(selection.Settings, "Render.Display.OutputResolution");
    const auto& output = measuredOutput.empty() ? run.Output : measuredOutput;
    ImGui::TextWrapped("%s%s%s", run.DisplayMode.c_str(), output.empty() ? "" : "   /   ", output.c_str());
    if (!outcome.empty())
    {
        const char* label = outcome == "completed" ? "Completed" : outcome.c_str();
        BistroPlot::ContinueRowIfFits(ImGui::CalcTextSize(label).x);
        ImGui::TextColored(ImGui::GetStyleColorVec4(outcome == "completed" ? ImGuiCol_NavCursor : ImGuiCol_Text), "%s", label);
    }
    ImGui::TextDisabled("%s", run.Timestamp.c_str());
    if (run.BaseBudget && run.AddedRoutes && run.Budget)
        ImGui::TextWrapped("Search budget %.1f s  /  base %.1f s  +  full reconstruction routes %.1f s",
            *run.Budget, *run.BaseBudget, *run.AddedRoutes);
    if (run.Elapsed) ImGui::TextDisabled("Elapsed %.1f s", *run.Elapsed);
    if (run.BaseBudget && ImGui::TreeNode("Search phases"))
    {
        for (size_t i = 0; i < run.Phases.size(); ++i)
        {
            const auto& phase = run.Phases[i];
            if (phase.Base && phase.Total)
                ImGui::TextWrapped("%s   %.1f s total  /  %.1f s base + %.1f s full routes",
                    SearchPhaseLabels[i], *phase.Total, *phase.Base, phase.Added.value_or(0.));
        }
        ImGui::TreePop();
    }
    DrawExportStatus(run, placement, selection, exportZip);
    ImGui::Spacing();
    if (winner)
    {
        const auto file = fs::u8path(Value(winner->Fields, "PresetFile"));
        const auto expected = run.Path.stem().wstring() + (winner == &run.Winners[0] ? L"-native.ini" : L"-upscaled.ini");
        if (file == expected)
        {
            if (ImGui::Button("Apply preset")) pendingPreset = run.Path.parent_path() / file;
        }
        else if (const auto& error = Value(winner->Fields, "PresetError"); !error.empty())
            ImGui::TextWrapped("Could not save preset: %s", error.c_str());
    }
    if (!run.RouteId.empty() && !hasTrace) ImGui::TextDisabled("Route   %s", run.RouteId.c_str());
    const auto timing = SummaryTiming(winner, selection);
    if (timing.Fps > 0 || timing.P95 > 0) DrawRouteMetrics(timing, _headingFont);
    if (target > 0 && timing.Fps > 0)
        ImGui::Text("Target   %.0f FPS   /   %s", target, run.TargetStatus(timing.Fps, winner));

    DrawPlacementComparison(placement ? *placement : run.Winners[0]);
    if (!run.Reconstructions.empty() && ImGui::CollapsingHeader("Reconstruction comparisons"))
        for (const auto& [id, group] : run.Reconstructions)
        {
            const auto title = group.Mode + " / " + group.Status + "##Reconstruction" + std::to_string(id);
            if (!ImGui::TreeNode(title.c_str())) continue;
            if (ImGui::BeginTable("Participants", int(ReconstructionColumns.size()), ImGuiTableFlags_BordersInnerH |
                ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
            {
                for (const auto* column : ReconstructionColumns) ImGui::TableSetupColumn(column);
                ImGui::TableHeadersRow();
                for (const auto& trial : group.Participants)
                {
                    ImGui::TableNextRow();
                    for (const auto& cell : ReconstructionRow(run, group, trial))
                    {
                        ImGui::TableNextColumn();
                        ImGui::TextWrapped("%s", cell.c_str());
                    }
                }
                ImGui::EndTable();
            }
            ImGui::TreePop();
        }

    if (hasTrace || (placement && (!placement->Placements[0].Fields.empty() || !placement->Placements[1].Fields.empty())))
    {
        if (ImGui::CollapsingHeader("Frame timing", ImGuiTreeNodeFlags_DefaultOpen))
        {
            _runFps = BistroPlot::Choice("", "Frame time (ms)", "FPS", _runFps);
            if (target > 0)
            {
                const float labelWidth = ImGui::CalcTextSize("Dashed target   60 FPS / 33.3 ms").x;
                BistroPlot::ContinueRowIfFits(labelWidth);
                ImGui::TextDisabled("Dashed target   %.0f FPS / %.1f ms", target, 1000. / target);
            }
            DrawPlot(RunLines(placement, selection, _placementPasses), _runFps, target);
        }
    }
    else if (!placement || winner)
        ImGui::TextWrapped("%s", selection.Error.empty() ? "Full-route frame trace unavailable." : selection.Error.c_str());

    if (winner)
    {
        ImGui::Spacing();
        ImGui::SeparatorText(winner == &run.Winners[0] ? "Selected preset / Native" : "Selected preset / Upscaled");
        if (const auto image = ImageSummary(winner->Settings); !image.empty()) ImGui::TextWrapped("%s", image.c_str());
    }
    if (placement)
    {
        if (placement == &run.Winners[1] && run.Winners[0].Confirmed() && placement->Confirmed())
            DrawTechniques(run.Winners[0], *placement, "Techniques compared with Native", "Native", "Upscaled");
        else if (!placement->Techniques.empty() && placement->Placements[1].Techniques.empty())
            DrawTechniques({}, *placement, "Measured techniques and owners", nullptr, "Measured value");
    }
    if (winner)
        DrawSettings(winner->Settings, run.Before, "Preset settings", "Selected preset");
    else if (hasTrace)
    {
        if (const auto image = ImageSummary(selection.Settings); !image.empty()) ImGui::TextWrapped("%s", image.c_str());
        if (!selection.Observed.Techniques.empty() || !selection.Observed.GpuSettings.empty())
            DrawTechniques({}, selection.Observed, "Measured techniques and owners", nullptr, "Measured value");
        DrawSettings(selection.Settings, run.Before, "Settings used for this pass", "Measured pass", "Before run", false,
            run.Mode == "Manual");
    }
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
    if (hasTrace && ImGui::TreeNode("Run details"))
    {
        const auto& measured = _runs[measuredRun].Passes[pass];
        ImGui::TextWrapped("%s", measured.Label.c_str());
        if (!run.Profile.empty()) ImGui::TextWrapped("Search scope   %s", run.Mode.c_str());
        if (!run.RouteId.empty()) ImGui::TextWrapped("Route   %s", run.RouteId.c_str());
        ImGui::TextDisabled("Interval measured %s", selection.AfterRender ? "after Render / Present" : "before Present");
        if (selection.Mode == "Async" && selection.ScopeMeanMs > 0 && selection.ScopeP95Ms > 0)
            ImGui::Text("Timed CPU scope  %.2f ms mean  |  %.2f ms P95", selection.ScopeMeanMs, selection.ScopeP95Ms);
        if (ImGui::SmallButton("Open pass CSV"))
            _openError = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", measured.Path.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32
                ? "Could not open the pass CSV." : "";
        if (!_openError.empty()) ImGui::TextUnformatted(_openError.c_str());
        if (winner && ImGui::TreeNode("Measured pass settings"))
        {
            DrawSettings(selection.Settings, _runs[measuredRun].Before);
            ImGui::TreePop();
        }
        ImGui::TreePop();
    }
}

int BistroComparePanel::DrawRunList(bool compact)
{
    ImGui::PushFont(_headingFont, ImGui::GetStyle().FontSizeBase * 1.45f);
    ImGui::TextUnformatted("Run history");
    ImGui::PopFont();
    BistroPlot::ContinueRowIfFits(ImGui::CalcTextSize("Refresh").x + 2.f * ImGui::GetStyle().FramePadding.x);
    if (ImGui::Button("Refresh")) Refresh(true);
    if (!_notice.empty()) ImGui::TextWrapped("%s", _notice.c_str());
    if (_runs.empty()) return -1;
    if (compact)
    {
        BistroPlot::ContinueRowIfFits(ImGui::CalcTextSize("Filters").x + 2.f * ImGui::GetStyle().FramePadding.x);
        if (ImGui::Button("Filters")) ImGui::OpenPopup("History filters");
    }
    if (!compact) ImGui::TextUnformatted("Filters");
    if (!compact || ImGui::BeginPopup("History filters"))
    {
        constexpr const char* labels[]{"All maps", "All modes", "All resolutions"};
        for (size_t i = 0; i < _filters.size(); ++i)
        {
            ImGui::PushID(labels[i]);
            ImGui::SetNextItemWidth(compact ? 14.f * ImGui::GetFontSize() : -1.f);
            if (ImGui::BeginCombo("##filter", _filters[i].empty() ? labels[i] : _filters[i].c_str()))
            {
                if (ImGui::Selectable(labels[i], _filters[i].empty())) _filters[i].clear();
                for (const auto& option : _filterOptions[i])
                    if (ImGui::Selectable(option.c_str(), option == _filters[i])) _filters[i] = option;
                ImGui::EndCombo();
            }
            ImGui::PopID();
        }
        if (compact) ImGui::EndPopup();
    }
    std::vector<int> visible;
    int selected = -1;
    for (int i = 0; i < int(_runs.size()); ++i)
    {
        const auto& run = _runs[i];
        if ((!_filters[0].empty() && _filters[0] != run.MapName) ||
            (!_filters[1].empty() && _filters[1] != run.DisplayMode) ||
            (!_filters[2].empty() && _filters[2] != run.Output)) continue;
        visible.push_back(i);
        if (_historyPath == run.Path) selected = i;
    }
    if (visible.empty())
    {
        ImGui::TextWrapped("No runs match these filters.");
        if (ImGui::Button("Clear filters")) _filters = {};
        return -1;
    }
    if (selected < 0) selected = visible.front();
    if (compact)
    {
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##Record", _runs[selected].Label.c_str()))
        {
            for (int i : visible)
            {
                ImGui::PushID(i);
                if (ImGui::Selectable(_runs[i].Label.c_str(), selected == i)) selected = i;
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
    }
    else
    {
        ImGui::Spacing();
        ImGui::TextDisabled("Recent runs   %zu", visible.size());
        if (ImGui::BeginChild("Recorded runs"))
        {
            ImGuiListClipper clipper;
            clipper.Begin(int(visible.size()));
            const float em = ImGui::GetFontSize();
            while (clipper.Step())
            {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
                {
                    const int i = visible[row];
                    const auto& run = _runs[i];
                    const bool active = i == selected;
                    ImGui::PushID(i);
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, active ? 2.f : 1.f);
                    ImGui::PushStyleColor(ImGuiCol_Border, ImGui::GetStyleColorVec4(active ? ImGuiCol_NavCursor : ImGuiCol_Border));
                    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(active ? ImGuiCol_HeaderActive : ImGuiCol_FrameBg));
                    if (ImGui::Button("##run", {-1.f, 4.85f * em})) selected = i;
                    ImGui::PopStyleColor(2);
                    ImGui::PopStyleVar();
                    const auto min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
                    const float x = min.x + (active ? 1.95f : .75f) * em, right = max.x - .7f * em;
                    auto* draw = ImGui::GetWindowDrawList();
                    if (active)
                    {
                        const ImVec2 mark{min.x + .97f * em, min.y + 1.12f * em};
                        draw->AddCircleFilled(mark, .46f * em, ImGui::GetColorU32(ImGuiCol_NavCursor));
                        const ImVec2 check[]{{mark.x - .24f * em, mark.y}, {mark.x - .06f * em, mark.y + .15f * em},
                            {mark.x + .24f * em, mark.y - .18f * em}};
                        draw->AddPolyline(check, 3, ImGui::GetColorU32(ImGuiCol_WindowBg), 0, .11f * em);
                    }
                    auto* font = ImGui::GetFont();
                    auto* heading = _headingFont ? _headingFont : font;
                    const auto text = [&](float y, float limit, const std::string& value, ImFont* face, float size, ImGuiCol color)
                    {
                        const ImVec2 pos{x, min.y + y * em};
                        const char* end = nullptr;
                        const float width = limit - x;
                        if (face->CalcTextSizeA(size, FLT_MAX, 0.f, value.c_str()).x > width)
                        {
                            const float dots = face->CalcTextSizeA(size, FLT_MAX, 0.f, "...").x;
                            const auto fit = face->CalcTextSizeA(size, std::max(0.f, width - dots), 0.f, value.c_str(), nullptr, &end);
                            draw->AddText(face, size, {x + fit.x, pos.y}, ImGui::GetColorU32(color), "...");
                        }
                        draw->AddText(face, size, pos, ImGui::GetColorU32(color), value.c_str(), end);
                    };
                    float titleRight = right;
                    if (run.SummaryFps && (run.FinalStatus.empty() || run.FinalStatus == "completed" || run.FinalStatus == "target reached" || run.FinalStatus == "reserve"))
                    {
                        char fps[32];
                        std::snprintf(fps, sizeof(fps), "%.1f FPS", *run.SummaryFps);
                        const float fpsX = right - heading->CalcTextSizeA(em, FLT_MAX, 0.f, fps).x;
                        if (fpsX > x + 5.f * em)
                        {
                            draw->AddText(heading, em, {fpsX, min.y + .69f * em}, ImGui::GetColorU32(ImGuiCol_CheckMark), fps);
                            titleRight = fpsX - .5f * em;
                        }
                    }
                    text(.48f, titleRight, run.MapName, heading, 1.28f * em, ImGuiCol_Text);
                    text(1.99f, right, run.DisplayMode + (run.Output.empty() ? "" : "  /  " + run.Output), font, em, ImGuiCol_Text);
                    text(3.25f, right, run.Timestamp, font, .93f * em, ImGuiCol_TextDisabled);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", run.Label.c_str());
                    ImGui::PopID();
                }
            }
        }
        ImGui::EndChild();
    }
    _historyPath = _runs[selected].Path;
    return selected;
}

void BistroComparePanel::DrawHistory(const fs::path& latestCompleted, fs::path& pendingPreset, ImFont* headingFont)
{
    _headingFont = headingFont;
    UpdateHistory(latestCompleted);
    if (_showComparison)
    {
        if (ImGui::Button("Back to run history")) _showComparison = false;
        BistroPlot::ContinueRowIfFits(ImGui::CalcTextSize("Compare passes").x * 1.5f);
        ImGui::PushFont(_headingFont, ImGui::GetStyle().FontSizeBase * 1.5f);
        ImGui::TextUnformatted("Compare passes");
        ImGui::PopFont();
        ImGui::Spacing();
        if (ImGui::BeginChild("Pass comparison", {}, ImGuiChildFlags_Borders))
            DrawComparison();
        ImGui::EndChild();
        return;
    }
    const float em = ImGui::GetFontSize();
    const auto room = ImGui::GetContentRegionAvail();
    const bool compact = room.x < 48.f * em || room.y < 19.f * em;
    int selected = -1;
    if (compact)
        selected = DrawRunList(true);
    else
    {
        const float width = std::clamp(room.x * .29f, 15.f * em, 24.f * em);
        if (ImGui::BeginChild("History browser", {width, 0}, ImGuiChildFlags_Borders))
            selected = DrawRunList(false);
        ImGui::EndChild();
        ImGui::SameLine();
    }
    if (ImGui::BeginChild("Run summary", {}, ImGuiChildFlags_Borders))
    {
        if (selected >= 0)
        {
            if (ImGui::GetStateStorage()->GetInt(ImGui::GetID("run"), -1) != selected)
            {
                ImGui::SetScrollY(0);
                ImGui::GetStateStorage()->SetInt(ImGui::GetID("run"), selected);
            }
            DrawRun(selected, pendingPreset);
        }
        else if (_runs.empty())
        {
            ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.5f);
            ImGui::TextUnformatted("No benchmark results yet");
            ImGui::PopFont();
            ImGui::TextWrapped("Start a benchmark from Setup to record a run.");
        }
    }
    ImGui::EndChild();
}

void BistroComparePanel::DrawComparison()
{
    if (_loadComparison)
    {
        for (auto& s : _selected) Select(s, s.RunIndex, s.PassIndex);
        _loadComparison = false;
    }
    const int columns = ImGui::GetContentRegionAvail().x >= 36.f * ImGui::GetFontSize() ? 2 : 1;
    if (ImGui::BeginTable("Recorded passes", columns, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV))
    {
        for (size_t i = 0; i < _selected.size(); ++i)
        {
            ImGui::TableNextColumn();
            DrawSelection(_selected[i], i);
        }
        ImGui::EndTable();
    }
    if (std::any_of(_selected.begin(), _selected.end(), [](const Selection& s) { return !s.Error.empty() || s.Samples.empty(); }))
    {
        ImGui::TextDisabled("Choose two completed passes with frame data to compare.");
        return;
    }
    ImGui::Spacing();
    _fps = BistroPlot::Choice("", "Frame time (ms)", "FPS", _fps);
    const auto& a = _selected[0];
    const auto& b = _selected[1];
    DrawPlot({PlotLine{&a, a.Mode}, PlotLine{&b, b.Mode}}, _fps);
    const auto version = Value(a.Settings, "RouteVersion");
    const bool sameRoute = Value(a.Settings, "MapId") == Value(b.Settings, "MapId") &&
        Value(a.Settings, "RouteId") == Value(b.Settings, "RouteId") &&
        !version.empty() && version == Value(b.Settings, "RouteVersion") &&
        Number(Value(a.Settings, "RouteSeconds")).value_or(-1) > 0 &&
        Number(Value(a.Settings, "RouteSeconds")) == Number(Value(b.Settings, "RouteSeconds")) &&
        Value(a.Settings, "PaintingHoldSeconds") == Value(b.Settings, "PaintingHoldSeconds");
    const bool sameWindow = !a.Range.Section.empty() && a.Range.Section == b.Range.Section &&
        a.Range.Begin >= 0 && a.Range.End > a.Range.Begin && a.Range.Begin == b.Range.Begin && a.Range.End == b.Range.End;
    const bool comparable = sameRoute && sameWindow && a.AfterRender == b.AfterRender &&
        !Value(a.Settings, "MeasurementVersion").empty() &&
        Value(a.Settings, "MeasurementVersion") == Value(b.Settings, "MeasurementVersion") &&
        a.Mode != "Unknown" && b.Mode != "Unknown";
    ImGui::SeparatorText("Run differences");
    if (!comparable)
        ImGui::TextWrapped("A/B deltas require matching routes, measured windows, timing boundaries and measurement versions.");
    const auto reference = "A / " + a.Mode, comparison = "B / " + b.Mode;
    DrawComparedTimings(a.Timing, b.Timing, reference.c_str(), comparison.c_str(), comparable);
    if ((a.ScopeMeanMs > 0 || b.ScopeMeanMs > 0) && ImGui::TreeNode("Timed CPU scope"))
    {
        if (BeginTimingTable("CPU scope metrics", reference.c_str(), comparison.c_str()))
        {
            MetricComparisonRow("CPU scope mean", a.ScopeMeanMs, b.ScopeMeanMs, comparable);
            MetricComparisonRow("CPU scope P95", a.ScopeP95Ms, b.ScopeP95Ms, comparable);
            ImGui::EndTable();
        }
        ImGui::TreePop();
    }
    if (!a.Observed.Techniques.empty() || !b.Observed.Techniques.empty() || !a.Observed.GpuSettings.empty() || !b.Observed.GpuSettings.empty())
        DrawTechniques(a.Observed, b.Observed, "Measured GPU placement and techniques", reference.c_str(), comparison.c_str());
    DrawSettings(_selected[1].Settings, _selected[0].Settings, "Recorded pass settings",
        comparison.c_str(), reference.c_str(), true);
}

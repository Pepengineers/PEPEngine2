#include "BistroInfoPanel.h"
#include "BistroIniMigration.h"
#include "BistroPlot.h"
#include "BistroBenchmark.h"
#include "Engine.Core/BenchmarkEngine.h"
#include "Engine.UI/UI_test.h"
#include "Engine.UI/PepengineTheme.h"
#include "../../../Resources/Resource.h"
#include <imgui/imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>

namespace Theme = Engine::UI::PepengineTheme;

namespace
{
constexpr ImVec4 Rgb(unsigned rgb)
{
    return {float((rgb >> 16) & 255) / 255.f, float((rgb >> 8) & 255) / 255.f, float(rgb & 255) / 255.f, 1.f};
}
bool PageButton(const char* label, bool selected, ImVec2 size)
{
    if (selected)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Ink);
        ImGui::PushStyleColor(ImGuiCol_Button, Theme::Lime);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Rgb(0xc7f496));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, Rgb(0x98cf66));
    }
    const bool pressed = ImGui::Button(label, size);
    if (selected) ImGui::PopStyleColor(4);
    return pressed;
}

}

void BistroInfoPanel::InitializeFont()
{
    Theme::Initialize(*ImGui::GetIO().Fonts, IDI_PEPENGINE2);
    _fullFont = Theme::BodyFont();
    _headingFont = Theme::HeadingFont();
    for (const auto& map : BistroRun::Maps)
        _backgrounds.emplace(map.Background, Theme::LoadResourceImage(*ImGui::GetIO().Fonts, map.Background));
}

void BistroInfoPanel::ShowSettings(bool openResults, const std::string& error)
{
    ResetMeasurementSeries();
    _displayError = error;
    _tab = openResults ? Tab::Results : Tab::Setup;
}

void BistroInfoPanel::ResetMeasurementSeries()
{
    _count = _next = 0;
    _hudMeanMs = _hudP95Ms = 0.f;
    _hudStatsDue = {};
    _gpuWork.clear();
    _timingEpoch = _packetCursor = 0;
}

void BistroInfoPanel::ApplyPreset(const std::filesystem::path& path)
{
    ConsoleVariablePatch patch;
    auto result = ConsoleModule::ReadPatchIni(path, patch, [](std::wstring& key) {
        MigrateBistroConfigKey(key);
        if (const auto page = LocalShadowPageSettingsIndex(key)) RegisterLocalShadowPageSettings(*page);
    });
    if (result.Error.empty())
        result = BenchmarkEngine::GetLocator().GetModule<ConsoleModule>()->ApplyPatch(patch);
    _displayError = result.Error.empty() ? "" : "Could not apply preset: " + result.Error;
}

namespace
{
struct RunMode
{
    const char* Name;
    const char* Description;
    BistroRun::Mode Kind;
    BistroPlacement::Profile Profile;
};

constexpr RunMode RunModes[] = {
    {"Manual", "One run with current settings.", BistroRun::Mode::Manual, BistroPlacement::Performance},
    {"Performance", "Search for settings targeting 60 FPS.", BistroRun::Mode::Profile, BistroPlacement::Performance},
    {"Quality", "Prioritize image quality at 30 FPS.", BistroRun::Mode::Profile, BistroPlacement::Quality},
};

void DrawModeCard(const RunMode& mode, BistroRun::Launch& launch, ImFont* heading, float height)
{
    const float em = ImGui::GetFontSize();
    const bool selected = launch.Kind == mode.Kind && (mode.Kind == BistroRun::Mode::Manual || mode.Profile == launch.Profile);
    const float width = ImGui::GetContentRegionAvail().x;
    const bool wide = width >= 25.f * em;
    const bool showIcon = width >= 19.f * em;
    const float inset = wide ? width * .305f : (showIcon ? 5.65f : 2.65f) * em;
    const float textWidth = std::max(em, width - inset - .8f * em);
    const float titleSize = std::min(1.68f * em,
        em * textWidth / std::max(em, heading->CalcTextSizeA(em, FLT_MAX, 0.f, mode.Name).x));
    const float descriptionSize = 1.10f * em;
    const float descriptionHeight = ImGui::GetFont()->CalcTextSizeA(descriptionSize, FLT_MAX, textWidth, mode.Description).y;
    const float textHeight = titleSize + .20f * em + descriptionHeight;
    height = std::max(height, textHeight + .65f * em);
    if (ImGui::InvisibleButton(mode.Name, {width, height}, ImGuiButtonFlags_EnableNav))
    {
        launch.Kind = mode.Kind;
        launch.Profile = mode.Profile;
    }
    const auto a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    auto* draw = ImGui::GetWindowDrawList();
    const auto accent = ImGui::GetColorU32(selected ? Theme::Lime : Theme::Mint);
    const bool hovered = ImGui::IsItemHovered();
    const bool focused = ImGui::GetIO().NavVisible && ImGui::IsItemFocused();
    const ImU32 fill = selected ? ImGui::GetColorU32(hovered ? ImGuiCol_HeaderHovered : ImGuiCol_HeaderActive) :
        ImGui::GetColorU32(hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg);
    draw->AddRectFilled(a, b, fill, .45f * em);
    draw->AddRect(a, b, focused ? ImGui::GetColorU32(ImGuiCol_NavCursor) : selected ? accent : ImGui::GetColorU32(ImGuiCol_Border),
        .45f * em, selected || focused ? .14f * em : 1.f);

    const float middle = (a.y + b.y) * .5f;
    const ImVec2 radio{a.x + (wide ? width * .058f : 1.22f * em), middle};
    draw->AddCircle(radio, .47f * em, selected ? accent : ImGui::GetColorU32(ImGuiCol_Text), 24, .11f * em);
    if (selected) draw->AddCircleFilled(radio, .25f * em, accent, 24);
    if (showIcon)
    {
        const float x = a.x + (wide ? width * .177f : 3.5f * em);
        const float iconScale = (wide ? 1.65f : 1.30f) * em;
        const auto p = [&](float dx, float dy) { return ImVec2{x + dx * iconScale, middle + dy * iconScale}; };
        const ImU32 icon = ImGui::GetColorU32(Theme::Mint);
        const float stroke = .10f * iconScale;
        if (mode.Kind == BistroRun::Mode::Manual)
            draw->AddTriangle(p(-.48f,-.68f), p(.65f, 0), p(-.48f,.68f), icon, stroke);
        else if (mode.Profile == BistroPlacement::Performance)
        {
            for (int i = 0; i < 3; ++i)
                draw->AddRect(p(-.65f + .5f * i, .13f - .43f * i), p(-.36f + .5f * i, .61f), icon, 0, stroke);
        }
        else
        {
            const ImVec2 layer[]{p(-.78f,-.25f), p(0,-.67f), p(.78f,-.25f), p(0,.17f)};
            draw->AddPolyline(layer, 4, icon, stroke, ImDrawFlags_Closed);
            draw->AddLine(p(-.78f,.18f), p(0,.62f), icon, stroke);
            draw->AddLine(p(0,.62f), p(.78f,.18f), icon, stroke);
        }
    }
    if (wide)
    {
        const float divider = a.x + width * .266f;
        const float half = std::min(height * .32f, 2.15f * em);
        draw->AddLine({divider, middle - half}, {divider, middle + half}, ImGui::GetColorU32(ImGuiCol_Border));
    }
    const float left = a.x + inset, top = middle - textHeight * .5f;
    draw->PushClipRect(a, b, true);
    draw->AddText(heading, titleSize, {left, top}, ImGui::GetColorU32(ImGuiCol_Text), mode.Name);
    draw->AddText(ImGui::GetFont(), descriptionSize, {left, top + titleSize + .20f * em},
        ImGui::GetColorU32(Rgb(0xcadbcf)), mode.Description, nullptr, textWidth);
    draw->PopClipRect();
}

void DrawRoutePreview(const BistroRun::Map& map, float height)
{
    if (std::string_view(map.Route) != "bistro-flight-v1")
    {
        ImGui::TextWrapped("Route: %s", map.Route);
        return;
    }
    using namespace BistroBenchmarkRoute;
    const float em = ImGui::GetFontSize();
    ImGui::Dummy({ImGui::GetContentRegionAvail().x, height});
    const auto a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(a, b, ImGui::GetColorU32(ImGuiCol_WindowBg), .35f * em);
    const auto grid = ImGui::GetColorU32(ImGuiCol_Border, .4f);
    const float radius = .35f * em;
    for (float x = a.x + em; x < b.x - radius; x += 1.25f * em) draw->AddLine({x, a.y + 1}, {x, b.y - 1}, grid);
    for (float y = a.y + em; y < b.y - radius; y += 1.25f * em) draw->AddLine({a.x + 1, y}, {b.x - 1, y}, grid);
    const struct { const Point* Points; size_t Count; } legs[]{
        {Red, std::size(Red)}, {Blue, std::size(Blue)}, {Green, std::size(Green)}, {Yellow, std::size(Yellow)}};
    // Project the flight's world-space XZ footprint in the same scale on both axes.
    ImVec2 low{FLT_MAX, FLT_MAX}, high{-FLT_MAX, -FLT_MAX};
    for (const auto& leg : legs)
        for (size_t i = 0; i < leg.Count; ++i)
        {
            const auto& p = leg.Points[i].Position;
            low.x = std::min(low.x, p.x); high.x = std::max(high.x, p.x);
            low.y = std::min(low.y, p.z); high.y = std::max(high.y, p.z);
        }
    const float scale = std::max(0.f, std::min((b.x - a.x - 2 * em) / (high.x - low.x),
        (b.y - a.y - 1.1f * em) / (high.y - low.y)));
    const auto project = [&](const Point& point) {
        return ImVec2{(a.x + b.x) * .5f + (point.Position.x - (high.x + low.x) * .5f) * scale,
            (a.y + b.y) * .5f - (point.Position.z - (high.y + low.y) * .5f) * scale};
    };
    const auto accent = ImGui::GetColorU32(Theme::Mint);
    for (const auto& leg : legs)
        for (size_t i = 1; i < leg.Count; ++i)
            draw->AddLine(project(leg.Points[i-1]), project(leg.Points[i]), accent, std::max(1.25f, .09f * em));
    draw->AddCircleFilled(project(Red[0]), .23f * em, ImGui::GetColorU32(Theme::Lime));
    draw->AddRect(a, b, ImGui::GetColorU32(ImGuiCol_Border), .35f * em);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Camera route viewed from above");
}

std::string OutputLabel(BistroPlacement::Size size)
{
    return std::to_string(size.first) + u8" × " + std::to_string(size.second);
}

void DrawOutputPicker(BistroPlacement::Size& output)
{
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (!ImGui::BeginCombo("##Output", OutputLabel(output).c_str())) return;
    std::vector<BistroPlacement::Size> sizes{{960,540}, {1280,720}, {1280,800}, {1600,900}, {1920,1080},
        {2560,1440}, {3840,2160}, output};
    const auto client = ImGui::GetMainViewport()->Size;
    if (client.x > 0 && client.y > 0) sizes.emplace_back(unsigned(client.x), unsigned(client.y));
    for (const auto& monitor : ImGui::GetPlatformIO().Monitors)
        if (monitor.MainSize.x > 0 && monitor.MainSize.y > 0)
            sizes.emplace_back(unsigned(monitor.MainSize.x), unsigned(monitor.MainSize.y));
    std::sort(sizes.begin(), sizes.end());
    sizes.erase(std::unique(sizes.begin(), sizes.end()), sizes.end());
    for (const auto& size : sizes)
    {
        const bool selected = size == output;
        if (ImGui::Selectable(OutputLabel(size).c_str(), selected)) output = size;
        if (selected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
}
}

void BistroInfoPanel::DrawLaunchControls(BistroBenchmark& benchmark)
{
    const float em = ImGui::GetFontSize(), gap = ImGui::GetStyle().ItemSpacing.y;
    const float actionHeight = 2.4f * em;
    ImGui::BeginDisabled(!benchmark.CanStart());
    if (ImGui::BeginChild("Setup choices", {}, ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground))
    {
        const float width = ImGui::GetContentRegionAvail().x;
        const bool columns = width >= 40.f * em;
        if (!_parametersOpen)
        {
            const float mapHeight = 17.7f * em + actionHeight + gap;
            const float panelHeight = std::max(mapHeight, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeight() - gap);
            const auto heading = [&](const char* name, float scale) {
                ImGui::PushFont(_headingFont, scale * ImGui::GetStyle().FontSizeBase);
                ImGui::TextUnformatted(name);
                ImGui::PopFont();
            };
            if (ImGui::BeginChild("Run mode", {columns ? std::floor(width * .57f) : 0.f, columns ? panelHeight : 17.7f * em},
                ImGuiChildFlags_Borders))
            {
                heading("Run mode", 1.52f);
                const float rowHeight = std::max(3.65f * em, std::floor((ImGui::GetContentRegionAvail().y - 2 * gap) / 3));
                for (const auto& mode : RunModes) DrawModeCard(mode, _launch, _headingFont, rowHeight);
            }
            ImGui::EndChild();
            if (columns) ImGui::SameLine();
            if (ImGui::BeginChild("Map and output", {0, columns ? panelHeight : mapHeight}, ImGuiChildFlags_Borders))
            {
                heading("Map and output", 1.52f);
                if constexpr (std::size(BistroRun::Maps) == 1)
                    heading(_launch.Level->Name, 1.08f);
                else
                {
                    heading("Map", 1.08f);
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (ImGui::BeginCombo("##Map", _launch.Level->Name))
                    {
                        for (const auto& map : BistroRun::Maps)
                        {
                            const bool selected = &map == _launch.Level;
                            if (ImGui::Selectable(map.Name, selected)) _launch.Level = &map;
                            if (selected) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                }
                const bool profile = _launch.Kind == BistroRun::Mode::Profile;
                constexpr auto outputLabel = "Output resolution";
                const auto& style = ImGui::GetStyle();
                const float pickerWidth = ImGui::CalcTextSize(OutputLabel(_launch.Output).c_str()).x +
                    ImGui::GetFrameHeight() + 2.f * style.FramePadding.x;
                const bool inlineOutput = ImGui::GetContentRegionAvail().x >= ImGui::CalcTextSize(outputLabel).x +
                    style.ItemSpacing.x + pickerWidth;
                float bottom = ImGui::GetFrameHeight() + actionHeight + 2 * gap;
                if (!inlineOutput) bottom += 1.08f * em + gap;
                if (profile) bottom += ImGui::GetFrameHeightWithSpacing();
#if defined(_DEBUG) || defined(DEBUG)
                bottom += ImGui::GetFrameHeightWithSpacing();
#endif
                DrawRoutePreview(*_launch.Level, std::max(2.8f * em, ImGui::GetContentRegionAvail().y - bottom));
                if (inlineOutput)
                {
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted(outputLabel);
                    ImGui::SameLine();
                }
                else heading(outputLabel, 1.08f);
                DrawOutputPicker(_launch.Output);
                if (profile) ImGui::Checkbox("Include upscalers in search", &_launch.AllowUpscalers);
                const char* action = !profile ? "Start benchmark" :
                    (_launch.Profile == BistroPlacement::Performance ? "Find Performance" : "Find Quality");
                ImGui::PushStyleColor(ImGuiCol_Button, Theme::Lime);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Rgb(0xc7f496));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, Rgb(0x98cf66));
                ImGui::PushStyleColor(ImGuiCol_Text, Theme::Ink);
                ImGui::PushFont(_headingFont, 0.f);
                if (ImGui::Button(action, {-FLT_MIN, actionHeight}))
                    benchmark.Launch(_launch, *BenchmarkEngine::GetLocator().GetModule<ConsoleModule>());
                ImGui::PopFont();
                ImGui::PopStyleColor(4);
#if defined(_DEBUG) || defined(DEBUG)
                if (ImGui::Button("Explore map (Debug)", {-FLT_MIN, 0})) _requestedDebugMap = _launch.Level;
#endif
            }
            ImGui::EndChild();
        }
        ImGui::SetNextItemOpen(_parametersOpen);
        _parametersOpen = ImGui::CollapsingHeader("Render parameters");
        if (_parametersOpen)
        {
            if (ImGui::BeginChild("Render parameter editor", {0, std::max(12.f * em, ImGui::GetContentRegionAvail().y)},
                ImGuiChildFlags_Borders))
                Engine::UI::DrawSettingsEditor();
            ImGui::EndChild();
        }
    }
    ImGui::EndChild();
    ImGui::EndDisabled();
}

void BistroInfoPanel::DrawSettings(RenderModule& render, BistroBenchmark& benchmark)
{
    if (!ImGui::GetCurrentContext()) return;
    if (!_initializedSize)
    {
        if (const auto* common = render.GetRenderPipelineCommonData(); common && common->WindowWidth && common->WindowHeight)
            _launch.Output = {common->WindowWidth, common->WindowHeight};
        _initializedSize = true;
    }
    _compare.UpdateHistory(benchmark.ReportPath());
    const auto* vp = ImGui::GetMainViewport();
    const float size = std::min(vp->WorkSize.x / 55.f, vp->WorkSize.y / 32.f);
    ImGui::PushFont(_fullFont, std::clamp(std::round(size), 18.f, 48.f));
    const float em = ImGui::GetFontSize();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {.95f * em, .68f * em});
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {.6f * em, .30f * em});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {.65f * em, .40f * em});
    Theme::Push();
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    if (ImGui::Begin("Benchmark", nullptr, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings))
    {
        auto* atlas = ImGui::GetIO().Fonts;
        ImFontAtlasRect photo;
        if (atlas->GetCustomRect(_backgrounds.at(_launch.Level->Background), &photo))
        {
            const auto min = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
            const ImVec2 max{min.x + size.x, min.y + size.y};
            const auto& uv0 = photo.uv0;
            const auto& uv1 = photo.uv1;
            float left = 0, top = .14f, right = 1, bottom = 1;
            const float imageAspect = photo.w / (photo.h * (bottom - top));
            const float targetAspect = size.x / std::max(1.f, size.y);
            if (targetAspect < imageAspect)
            {
                left = .5f * (1 - targetAspect / imageAspect);
                right = 1 - left;
            }
            else
            {
                const float inset = (bottom - top) * (1 - imageAspect / targetAspect) * .5f;
                top += inset;
                bottom -= inset;
            }
            auto* draw = ImGui::GetWindowDrawList();
            draw->PushClipRectFullScreen();
            const auto uv = [&](float x, float y) {
                return ImVec2{uv0.x + x * (uv1.x - uv0.x), uv0.y + y * (uv1.y - uv0.y)};
            };
            draw->AddImage(atlas->TexRef, min, max, uv(left, top), uv(right, bottom), IM_COL32(150, 161, 151, 255));
            const auto shade = IM_COL32(10, 23, 19, _tab == Tab::Results ? 236 : 215);
            const auto highlight = IM_COL32(10, 23, 19, _tab == Tab::Results ? 229 : 165);
            draw->AddRectFilledMultiColor(min, max, shade, highlight, highlight, shade);
            draw->PopClipRect();
        }
        const float width = ImGui::GetContentRegionAvail().x;
        const ImVec2 start = ImGui::GetCursorPos();
        const float navHeight = 2.60f * em;
        const float baseSize = ImGui::GetStyle().FontSizeBase;
        ImGui::BeginGroup();
        Theme::DrawBrand(2.55f * em);
        if (width >= 29.f * em)
        {
            ImGui::SameLine(0.f, .8f * em);
            const auto line = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddLine({line.x, line.y + .3f * em}, {line.x, line.y + 2.25f * em},
                ImGui::GetColorU32(ImGuiCol_Separator));
            ImGui::SetCursorScreenPos({line.x + .82f * em, line.y + .65f * em});
            ImGui::PushFont(_fullFont, 1.1f * baseSize);
            ImGui::TextUnformatted("Benchmark");
            ImGui::PopFont();
        }
        ImGui::EndGroup();
        const ImVec2 titleSize = ImGui::GetItemRectSize();
        const float navWidth = std::clamp(width * .125f, 5.1f * em, 8.f * em);
        const float navGap = ImGui::GetStyle().ItemSpacing.x * .35f;
        const bool historyToggle = _tab == Tab::Results && _compare.CanToggleHistory();
        const float navSpan = 2.f * navWidth + navGap + (historyToggle ? navHeight + navGap : 0.f);
        if (titleSize.x + navSpan + navGap < width)
            ImGui::SetCursorPos({start.x + width - navSpan, start.y + (titleSize.y - navHeight) * .5f});
        ImGui::PushFont(_headingFont, 1.12f * baseSize);
        if (historyToggle)
        {
            _compare.DrawHistoryToggle(navHeight);
            ImGui::SameLine(0.f, navGap);
        }
        if (PageButton("Setup", _tab == Tab::Setup, {navWidth, navHeight})) _tab = Tab::Setup;
        ImGui::SameLine(0.f, navGap);
        if (PageButton("Results", _tab == Tab::Results, {navWidth, navHeight})) _tab = Tab::Results;
        ImGui::PopFont();
        ImGui::Separator();
        if (!_displayError.empty())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 180, 146, 255));
            ImGui::TextWrapped("%s", _displayError.c_str());
            ImGui::PopStyleColor();
        }
        else if (!benchmark.Message().empty() && benchmark.Message() != "completed")
            ImGui::TextWrapped("%s", benchmark.Message().c_str());
        if (_tab == Tab::Setup) DrawLaunchControls(benchmark);
        else
        {
            std::filesystem::path preset;
            _compare.DrawHistory(benchmark.ReportPath(), preset, _headingFont);
            if (!preset.empty()) ApplyPreset(preset);
        }
    }
    ImGui::End();
    Theme::Pop();
    ImGui::PopStyleVar(3);
    ImGui::PopFont();
}

void BistroInfoPanel::RecordPresent(std::chrono::steady_clock::time_point begin, std::chrono::steady_clock::time_point now,
    BistroRender::QueueSubmission& submissions, bool manualGpuTimings)
{
    // The Windows steady clock and DX12 GetClockCalibration both use the QPC epoch.
    const double beginMs = std::chrono::duration<double, std::milli>(begin.time_since_epoch()).count();
    if (!_count) _measurementBeginMs = beginMs;
    _presentBeginMs[_next] = beginMs;
    _presentMs[_next] = std::chrono::duration<float, std::milli>(now - begin).count();
    _next = (_next + 1) % _presentMs.size();
    _count = std::min(_count + 1, _presentMs.size());
    if (now >= _hudStatsDue)
    {
        auto samples = _presentMs;
        const auto end = samples.begin() + _count;
        _hudMeanMs = float(std::accumulate(samples.begin(), end, 0.0) / _count);
        const auto p95 = samples.begin() + (_count * 95 - 1) / 100;
        std::nth_element(samples.begin(), p95, end);
        _hudP95Ms = *p95;
        _hudStatsDue = now + std::chrono::milliseconds(250);
    }
    const double first = _presentBeginMs[_count == _presentMs.size() ? _next : 0];
    for (const auto& packet : submissions.ReadBenchmarkIntervals(_timingEpoch, _packetCursor, manualGpuTimings))
    {
        if (packet.BeginMs < _measurementBeginMs || packet.EndMs <= first) continue;
        auto [device, added] = _gpuWork.try_emplace(packet.Device);
        if (added) device->second.Adapter = submissions.AdapterLabel(packet.Device);
        auto& intervals = device->second.Intervals;
        // Keep the union of retired packets, including delayed and overlapping queue submissions.
        auto it = std::lower_bound(intervals.begin(), intervals.end(), packet.BeginMs,
            [](const auto& interval, double begin) { return interval.second < begin; });
        double begin = packet.BeginMs, end = packet.EndMs;
        if (it == intervals.end() || end < it->first)
            intervals.insert(it, {begin, end});
        else
        {
            begin = std::min(begin, it->first);
            end = std::max(end, it->second);
            auto next = it + 1;
            while (next != intervals.end() && next->first <= end)
            {
                end = std::max(end, next->second);
                ++next;
            }
            *it = {begin, end};
            intervals.erase(it + 1, next);
        }
    }
    for (auto& [id, gpu] : _gpuWork)
    {
        auto& intervals = gpu.Intervals;
        intervals.erase(intervals.begin(), std::lower_bound(intervals.begin(), intervals.end(), first,
            [](const auto& interval, double cutoff) { return interval.second < cutoff; }));
    }
}

void BistroInfoPanel::DrawPresentChart(float height) const
{
    const auto first = _count == _presentMs.size() ? _next : 0;
    const float peak = std::max(5.f, 5.f * std::ceil(*std::max_element(_presentMs.begin(), _presentMs.begin() + _count) / 5.f));
    const float em = ImGui::GetFontSize();
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float width = std::max(1.f, ImGui::GetContentRegionAvail().x);
    char scale[48];
    std::snprintf(scale, sizeof(scale), "%.0f ms", peak);
    const float labelWidth = std::min(width * .3f, ImGui::CalcTextSize(scale).x + .45f * em);
    const ImVec2 origin{start.x + labelWidth, start.y}, end{start.x + width, start.y + height};
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, end, ImGui::GetColorU32(ImGuiCol_FrameBg));
    const auto grid = ImGui::GetColorU32(ImGuiCol_Border);
    for (int i = 0; i <= 6; ++i)
    {
        float x = origin.x + (end.x - origin.x) * (i / 6.f);
        draw->AddLine({x, origin.y}, {x, end.y}, grid);
    }
    for (int i = 0; i <= 2; ++i)
    {
        float y = origin.y + (end.y - origin.y) * (i / 2.f);
        draw->AddLine({origin.x, y}, {end.x, y}, grid);
    }
    const auto muted = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    draw->AddText(start, muted, scale);
    draw->AddText({start.x, end.y - em}, muted, "0 ms");
    const auto point = [&](size_t i, double ms) {
        return ImVec2{origin.x + (end.x - origin.x) * float(i) / float(std::max<size_t>(1, _count - 1)),
            end.y - height * std::clamp(float(ms) / peak, 0.f, 1.f)};
    };
    const auto presentColor = ImGui::GetColorU32(Theme::Paper);
    for (size_t i = 1; i < _count; ++i)
        draw->AddLine(point(i - 1, _presentMs[(first + i - 1) % _presentMs.size()]),
            point(i, _presentMs[(first + i) % _presentMs.size()]), presentColor, 1.5f);
    ImGui::Dummy({width, height});
    ImGui::Text("Present interval  /  last %zu frames", _count);
    for (const auto& [id, gpu] : _gpuWork)
    {
        if (gpu.Intervals.empty()) continue;
        const ImColor color = id == 0 ? ImColor(Theme::Mint) : ImColor::HSV(std::fmod(.12f + .382f * (id - 1), 1.f), .58f, .98f);
        constexpr const char* label = "  /  work";
        BistroPlot::ContinueRowIfFits(ImGui::CalcTextSize(gpu.Adapter.c_str()).x + ImGui::CalcTextSize(label).x);
        ImGui::TextColored(color, "%s%s", gpu.Adapter.c_str(), label);
        auto interval = gpu.Intervals.begin();
        std::optional<ImVec2> previous;
        for (size_t i = 0; i < _count; ++i)
        {
            const auto frame = (first + i) % _presentMs.size();
            const double begin = _presentBeginMs[frame], frameEnd = begin + _presentMs[frame];
            while (interval != gpu.Intervals.end() && interval->second <= begin) ++interval;
            double work = 0;
            for (auto it = interval; it != gpu.Intervals.end() && it->first < frameEnd; ++it)
                work += std::min(frameEnd, it->second) - std::max(begin, it->first);
            if (work <= 0) { previous.reset(); continue; }
            const auto p = point(i, work);
            if (previous) draw->AddLine(*previous, p, color, 1.7f);
            else draw->AddCircleFilled(p, 1.5f, color);
            previous = p;
        }
    }
}

void BistroInfoPanel::DrawMeasurement(const BistroBenchmark& benchmark, RECT visibleClient) const
{
    if (!ImGui::GetCurrentContext() || IsRectEmpty(&visibleClient)) return;
    const auto* vp = ImGui::GetMainViewport();
    const float visibleWidth = float(visibleClient.right - visibleClient.left);
    const float visibleHeight = float(visibleClient.bottom - visibleClient.top);
    const float size = std::clamp(std::round(visibleHeight / 40.f), 14.f, 36.f);
    ImGui::PushFont(_fullFont, size);
    Theme::Push();
    const float em = ImGui::GetFontSize();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {.9f * em, .6f * em});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {.7f * em, .22f * em});
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::SetNextWindowPos({vp->Pos.x + visibleClient.left, vp->Pos.y + visibleClient.bottom}, ImGuiCond_Always, {0,1});
    ImGui::SetNextWindowSize({visibleWidth, 0});
    ImGui::SetNextWindowBgAlpha(.96f);
    if (ImGui::Begin("##Measurement", nullptr, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings))
    {
        const float left = ImGui::GetCursorScreenPos().x, right = left + ImGui::GetContentRegionAvail().x;
        const auto top = ImGui::GetCursorScreenPos().y;
        const bool wide = right - left >= 37.f * em;
        Theme::DrawBrand(1.65f * em);
        const auto phase = benchmark.HudPhase();
        if (wide && ImGui::GetItemRectMax().x + ImGui::CalcTextSize(phase.c_str()).x + em < right)
        {
            ImGui::SameLine();
            ImGui::SetCursorScreenPos({ImGui::GetCursorScreenPos().x, top + .30f * em});
        }
        ImGui::TextWrapped("%s", phase.c_str());
        const int seconds = int(std::ceil(std::max(0., benchmark.HudRemainingSeconds())));
        char clock[64];
        if (const auto phaseTime = benchmark.HudPhaseRemainingSeconds())
        {
            const int phaseSeconds = int(std::ceil(*phaseTime));
            std::snprintf(clock, sizeof(clock), "%d:%02d phase  /  %d:%02d search  |  Esc to cancel",
                phaseSeconds / 60, phaseSeconds % 60, seconds / 60, seconds % 60);
        }
        else std::snprintf(clock, sizeof(clock), "%d:%02d remaining    |    Esc to cancel", seconds / 60, seconds % 60);
        const float clockWidth = ImGui::CalcTextSize(clock).x;
        if (ImGui::GetItemRectMax().x + clockWidth + em < right)
        {
            ImGui::SameLine();
            ImGui::SetCursorScreenPos({right - clockWidth, ImGui::GetItemRectMin().y});
        }
        ImGui::TextUnformatted(clock);
        ImGui::Separator();
        if (benchmark.HudShowsMeasurements() && _count && _hudMeanMs > 0)
        {
            ImGui::BeginGroup();
            ImGui::TextDisabled("Mean frame time");
            ImGui::PushFont(_headingFont, 1.35f * ImGui::GetStyle().FontSizeBase);
            ImGui::TextColored(Theme::Lime, "%.1f ms", _hudMeanMs);
            ImGui::PopFont();
            ImGui::Text("FPS  %.1f     P95  %.1f ms", 1000.f / _hudMeanMs, _hudP95Ms);
            ImGui::EndGroup();
            if (wide) ImGui::SameLine(0.f, 1.2f * em);
            ImGui::BeginGroup();
            DrawPresentChart(std::min(8.5f * em, .25f * visibleHeight));
            ImGui::EndGroup();
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    Theme::Pop();
    ImGui::PopFont();
}

#include "BistroInfoPanel.h"
#include "BistroIniMigration.h"
#include "BistroPlot.h"
#include "App.Base/Window.h"
#include "BistroBenchmark.h"
#include "Engine.Core/BenchmarkEngine.h"
#include "Engine.UI/UI_test.h"
#include <imgui/imgui.h>
#include <algorithm>
#include <cfloat>
#include <cmath>

namespace
{
constexpr ImVec4 Accent{.98f, .73f, .29f, 1.f};
constexpr std::pair<ImGuiCol, ImVec4> FullPalette[] = {
    {ImGuiCol_CheckMark, {.26f, .87f, .81f, 1.f}},
    {ImGuiCol_Button, {.13f, .24f, .28f, 1.f}},
    {ImGuiCol_ButtonHovered, {.17f, .34f, .38f, 1.f}},
    {ImGuiCol_ButtonActive, {.13f, .43f, .44f, 1.f}},
    {ImGuiCol_FrameBg, {.12f, .16f, .20f, 1.f}},
    {ImGuiCol_Header, {.13f, .20f, .24f, 1.f}},
    {ImGuiCol_HeaderHovered, {.16f, .30f, .34f, 1.f}},
    {ImGuiCol_HeaderActive, {.14f, .36f, .38f, 1.f}},
    {ImGuiCol_Tab, {.08f, .11f, .14f, 1.f}},
    {ImGuiCol_TabHovered, {.15f, .28f, .31f, 1.f}},
    {ImGuiCol_TabSelected, {.13f, .24f, .27f, 1.f}},
    {ImGuiCol_TabSelectedOverline, {.26f, .87f, .81f, 1.f}},
    {ImGuiCol_TableHeaderBg, {.15f, .19f, .23f, 1.f}},
    {ImGuiCol_TextDisabled, {.67f, .72f, .76f, 1.f}},
};
using BistroBenchmarkData::RunMode;
ImVec4 TraceColor(RunMode mode)
{
    switch (mode)
    {
    case RunMode::Manual: return {.36f, .88f, .65f, 1.f};
    case RunMode::SingleGPU: return {.36f, .75f, 1.f, 1.f};
    case RunMode::Async: return {1.f, .72f, .24f, 1.f};
    case RunMode::MultiGPU: return {.85f, .57f, 1.f, 1.f};
    }
    return {1,1,1,1};
}
using Action = BistroBenchmark::Action;

void Heading(const char* label) { ImGui::TextColored(Accent, "%s", label); }

float ButtonWidth(const char* label)
{
    return ImGui::CalcTextSize(label).x + 2.f * ImGui::GetStyle().FramePadding.x;
}

void EnumSelector(const char* label, IConsoleVariable& setting, std::string& error)
{
    const auto current = setting.GetInt();
    ImGui::TextDisabled("%s", label);
    ImGui::SetNextItemWidth(-1);
    ImGui::PushID(label);
    if (ImGui::BeginCombo("##value", std::string(setting.GetEnumValueName(current)).c_str()))
    {
        const int first = setting.GetEnumFirstValue();
        for (int i = first; i < first + setting.GetEnumCount(); ++i)
        {
            const auto name = std::string(setting.GetEnumValueName(i));
            if (ImGui::Selectable(name.c_str(), i == current) && i != current)
                error = setting.SetFromString(std::to_string(i)) ? "" : std::string("Could not apply ") + label;
        }
        ImGui::EndCombo();
    }
    ImGui::PopID();
}

void ProfileButtons(BistroBenchmark& benchmark, Action action)
{
    if (ImGui::Button("Find performance preset")) benchmark.Launch(action, BistroPlacement::Performance);
    BistroPlot::ContinueRowIfFits(ButtonWidth("Find quality preset"));
    if (ImGui::Button("Find quality preset")) benchmark.Launch(action, BistroPlacement::Quality);
}
}

void BistroInfoPanel::InitializeFont()
{
    auto* atlas = ImGui::GetIO().Fonts;
    if (atlas->Fonts.empty()) atlas->AddFontDefault();
    wchar_t windowsDir[MAX_PATH]{};
    const auto length = GetWindowsDirectoryW(windowsDir, MAX_PATH);
    if (length && length < MAX_PATH)
    {
        const auto path = std::filesystem::path(windowsDir) / L"Fonts" / L"segoeui.ttf";
        ImFontConfig config;
        config.Flags |= ImFontFlags_NoLoadError;
        _fullFont = atlas->AddFontFromFileTTF(path.u8string().c_str(), 16.f, &config);
    }
    if (!_fullFont) _fullFont = atlas->AddFontDefaultVector();
}

void BistroInfoPanel::RefreshPlacement(const BistroRenderPipeline& pipeline)
{
    _placement.clear();
    _groups.clear();
    _peers.clear();
    const auto& devices = pipeline.BenchmarkSubmissions();
    _primary = devices.AdapterLabel(0);
    for (int index : devices.BenchmarkAdapterIndices()) _peers.push_back(devices.AdapterLabel(index + 1));

    std::map<std::pair<std::string, std::string>, unsigned> observed;
    const auto mark = [&](const std::string& technique, const std::string& adapter, BistroRender::Queue queue)
    {
        observed[{technique, adapter}] |= 1u << unsigned(queue);
    };
    constexpr std::array names{"AO", "Environment", "LightGrid", "Particle", "Water", "CloudPicture"};
    for (const auto& packet : pipeline.LastSchedule().Packets)
    {
        const auto group = BistroBenchmark::QueueGroup(packet.Name);
        if (packet.Name == "cloud picture GPU0 import") mark("Cloud picture import", _primary, packet.Target);
        else if (group >= 0) mark(names[group], _primary, packet.Target);
        else if (packet.Name == "Game HUD raster") mark("GameHUD", _primary, packet.Target);
        else if (packet.Name.find("world-space canvases") == 0) mark("World canvases", _primary, packet.Target);
    }
    const auto add = [&](const std::string& name, const std::string& adapter, unsigned mask)
    {
        std::string queue;
        constexpr std::array labels{"DIRECT", "COMPUTE", "COPY"};
        for (unsigned q = 0; q < labels.size(); ++q)
            if (mask & (1u << q)) { if (!queue.empty()) queue += " + "; queue += labels[q]; }
        _placement.push_back({name, adapter, queue});
        if (adapter == "inactive") return;
        auto& list = _groups[{adapter, queue}];
        if (!list.empty()) list += ", ";
        list += name;
    };
    for (auto [name, adapter] : pipeline.PanelAdapters())
    {
        constexpr std::string_view suffix = "Adapter";
        if (name.size() >= suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
            name.resize(name.size() - suffix.size());
        const auto key = std::make_pair(name, adapter);
        const auto mask = observed[key];
        observed.erase(key);
        if (name == "CloudPicture" && pipeline.HasRemoteCloudPicture())
            add("Cloud picture generation", adapter, 1u << unsigned(BistroRender::Queue::Compute));
        else
            add(name, adapter, mask);
    }
    for (const auto& [key, mask] : observed) add(key.first, key.second, mask);
}

void BistroInfoPanel::DrawPlacement(bool compact) const
{
    if (compact)
    {
        for (const auto& [key, names] : _groups)
        {
            const auto shortId = key.first.substr(0, key.first.find(']') + 1);
            ImGui::TextDisabled("GPU %s %s", shortId.c_str(), key.second.c_str());
            ImGui::SameLine();
            ImGui::TextWrapped("%s", names.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%s", key.first.c_str(), names.c_str());
        }
        return;
    }
    if (!ImGui::BeginTable("Technique placement", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp, {0, ImGui::GetTextLineHeightWithSpacing() * 10}))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Technique", ImGuiTableColumnFlags_WidthStretch, 1.f);
    ImGui::TableSetupColumn("GPU", ImGuiTableColumnFlags_WidthStretch, 1.6f);
    ImGui::TableSetupColumn("Queue", ImGuiTableColumnFlags_WidthStretch, 1.f);
    ImGui::TableHeadersRow();
    for (const auto& row : _placement)
    {
        ImGui::TableNextColumn(); ImGui::TextWrapped("%s", row.Technique.c_str());
        ImGui::TableNextColumn(); ImGui::TextWrapped("%s", row.Adapter.c_str());
        ImGui::TableNextColumn(); ImGui::TextWrapped("%s", row.Queue.empty() ? "-" : row.Queue.c_str());
    }
    ImGui::EndTable();
}

void BistroInfoPanel::ApplyResolution(Window& window)
{
    if (!_pendingPreset.empty())
    {
        const auto path = std::exchange(_pendingPreset, {});
        ConsoleVariablePatch patch;
        auto result = ConsoleModule::ReadPatchIni(path, patch, MigrateBistroIniKey);
        if (result.Error.empty())
            result = BenchmarkEngine::GetLocator().GetModule<ConsoleModule>()->ApplyPatch(patch);
        if (!result.Error.empty())
        {
            _displayError = "Could not apply preset: " + result.Error;
            _pendingSize = {};
            return;
        }
        _displayError.clear();
    }
    if (!_pendingSize.first) return;
    const auto [width, height] = _pendingSize;
    _pendingSize = {};
    if (IsZoomed(window.GetWindowHandle())) ShowWindow(window.GetWindowHandle(), SW_RESTORE);
    window.SetWindowSize(width, height);
}

void BistroInfoPanel::DrawCurrentSettings(RenderModule& render)
{
    const auto& common = *render.GetRenderPipelineCommonData();
    const auto console = BenchmarkEngine::GetLocator().GetModule<ConsoleModule>();
    auto& provider = console->FindConsoleVariable(L"Render.Upscaler.Provider");
    auto& quality = console->FindConsoleVariable(L"Render.Upscaler.Quality");
    const auto dimensions = [](unsigned w, unsigned h) { return std::to_string(w) + " x " + std::to_string(h); };
    const float width = std::min(ImGui::GetContentRegionAvail().x, 54.f * ImGui::GetFontSize());
    if (ImGui::BeginTable("Output settings", width < 25.f * ImGui::GetFontSize() ? 1 : 3, ImGuiTableFlags_SizingStretchSame, {width, 0}))
    {
        ImGui::TableNextColumn();
        ImGui::TextDisabled("Output resolution");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##resolution", dimensions(common.WindowWidth, common.WindowHeight).c_str()))
        {
            using Size = std::pair<uint16_t, uint16_t>;
            const Size current{uint16_t(common.WindowWidth), uint16_t(common.WindowHeight)};
            std::vector<Size> sizes{{960,540}, {1280,720}, {1280,800}, {1600,900}, {1920,1080}, {2560,1440}, {3840,2160}, current};
            std::sort(sizes.begin(), sizes.end());
            sizes.erase(std::unique(sizes.begin(), sizes.end()), sizes.end());
            for (const auto& size : sizes)
                if (ImGui::Selectable(dimensions(size.first, size.second).c_str(), size == current) && size != current)
                    _pendingSize = size;
            ImGui::EndCombo();
        }
        ImGui::TableNextColumn(); EnumSelector("Upscaler", provider, _displayError);
        ImGui::TableNextColumn(); EnumSelector("Upscaler quality", quality, _displayError);
        ImGui::EndTable();
    }
    const auto& status = render.GetReconstructionStatus();
    ImGui::TextDisabled("Internal render %u x %u", common.DownscaledWidth, common.DownscaledHeight);
    if (status.Requested.Provider != status.Effective)
        ImGui::TextWrapped("Effective: %s  %s", std::string(provider.GetEnumValueName(int(status.Effective))).c_str(),
            status.FallbackReason.c_str());
}

void BistroInfoPanel::DrawLivePlot() const
{
    ImGui::Text("Live   %.1f FPS    %.1f ms", _displayFps, _displayFps > 0 ? 1000.f / _displayFps : 0.f);
    ImGui::PushStyleColor(ImGuiCol_PlotLines, TraceColor(RunMode::Manual));
    ImGui::PlotLines("##live", _frameMs.data(), int(_count), _count == _frameMs.size() ? int(_next) : 0,
        nullptr, 0.f, FLT_MAX, {-1, 32});
    ImGui::PopStyleColor();
}

void BistroInfoPanel::DrawResult(const BistroBenchmark& benchmark, RunMode tab)
{
    const auto latestSlot = benchmark.LatestResultSlot();
    const auto& latest = benchmark.LastPass(latestSlot);
    _compare.UpdateHistory(benchmark.ReportPath());
    if (_latestResult != latest.Path)
    {
        _latestResult = latest.Path;
        if (latest.Mode == RunMode::SingleGPU || latest.Mode == RunMode::Async)
            _queueResults = latest.Mode == RunMode::Async;
        if (latest.Mode == RunMode::SingleGPU || latest.Mode == RunMode::MultiGPU)
        {
            auto& quality = latest.Mode == RunMode::SingleGPU ? _singleQuality : _multiQuality;
            quality = latestSlot == BistroBenchmark::ResultSlot(latest.Mode, BistroPlacement::Quality);
        }
    }
    ImGui::SeparatorText(tab == RunMode::Manual ? "Last completed pass" : "Search results");
    ImGui::PushID("result filters");
    auto mode = tab;
    if (tab == RunMode::SingleGPU)
    {
        if (!_queueResults.has_value()) _queueResults = _compare.LatestSingleGpuIsAsync();
        _queueResults = BistroPlot::Choice("View results", "Presets", "Queue search", *_queueResults);
        if (*_queueResults) mode = RunMode::Async;
    }
    const char* profile = "";
    if (mode == RunMode::SingleGPU || mode == RunMode::MultiGPU)
    {
        auto& quality = mode == RunMode::SingleGPU ? _singleQuality : _multiQuality;
        if (!quality.has_value()) quality = _compare.LatestQuality(ModeName(mode));
        quality = BistroPlot::Choice("Saved profile", "Performance", "Quality", *quality, mode == RunMode::SingleGPU);
        profile = BistroPlacement::ProfileName[*quality ? BistroPlacement::Quality : BistroPlacement::Performance];
    }
    ImGui::PopID();
    _compare.DrawLatest(ModeName(mode), profile, _plotFps, benchmark.CanStart(), _pendingPreset, _pendingSize);
    if (!_displayError.empty()) ImGui::TextWrapped("%s", _displayError.c_str());
}
void BistroInfoPanel::DrawFull(RenderModule& render, BistroBenchmark& benchmark)
{
    const float em = ImGui::GetFontSize();
    const float left = ImGui::GetCursorPosX();
    const bool wideBar = ImGui::GetContentRegionAvail().x >= 52.f * em;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Bistro benchmark");
    if (wideBar) ImGui::SameLine(0.f, 1.2f * em);
    else
    {
        BistroPlot::ContinueRowIfFits(ButtonWidth("Settings [F1]"));
        if (ImGui::SmallButton("Settings [F1]")) Engine::UI::OpenSettings();
        BistroPlot::ContinueRowIfFits(ButtonWidth("Compact [F6]"));
        if (ImGui::SmallButton("Compact [F6]")) _compact = true;
    }
    struct Tab { const char* Label; std::optional<RunMode> Mode; };
    constexpr Tab tabs[]{{"Current run", RunMode::Manual}, {"Single GPU", RunMode::SingleGPU},
                         {"Multi-GPU", RunMode::MultiGPU}, {"Compare", std::nullopt}};
    const Tab* active = nullptr;
    if (ImGui::BeginTabBar("Benchmark modes", ImGuiTabBarFlags_DrawSelectedOverline | ImGuiTabBarFlags_FittingPolicyScroll))
    {
        for (const auto& tab : tabs)
            if (ImGui::BeginTabItem(tab.Label))
            {
                active = &tab;
                ImGui::EndTabItem();
            }
        if (wideBar)
        {
            if (ImGui::TabItemButton("Settings [F1]", ImGuiTabItemFlags_Trailing)) Engine::UI::OpenSettings();
            if (ImGui::TabItemButton("Compact [F6]", ImGuiTabItemFlags_Trailing)) _compact = true;
        }
        ImGui::EndTabBar();
    }
    if (!active) return;

    // Keep the navigation in view and remember the scroll position within each tab.
    ImGui::SetCursorPosX(left);
    if (ImGui::BeginChild(active->Label))
    {
        if (!benchmark.Message().empty()) ImGui::TextWrapped("%s", benchmark.Message().c_str());
        if (active->Mode == RunMode::Manual)
        {
            ImGui::SeparatorText("Current configuration");
            ImGui::TextWrapped("Scene GPU   %s", _primary.c_str());
            ImGui::BeginDisabled(!benchmark.CanStart());
            DrawCurrentSettings(render);
            if (ImGui::Button("Run one pass")) benchmark.Launch(Action::PlacementRun);
            ImGui::EndDisabled();
            BistroPlot::ContinueRowIfFits(23.f * em);
            ImGui::TextWrapped("Route %.0f s   /   Warmup %g s   /   At each painting %g s",
                benchmark.FlightTime(), benchmark.BaseWarmup(), benchmark.PaintingHold());

            if (ImGui::GetContentRegionAvail().x >= 82.f * em &&
                ImGui::BeginTable("Current run overview", 2, ImGuiTableFlags_SizingStretchProp))
            {
                ImGui::TableSetupColumn("Live placement", ImGuiTableColumnFlags_WidthStretch, .33f);
                ImGui::TableSetupColumn("Completed pass", ImGuiTableColumnFlags_WidthStretch, .67f);
                ImGui::TableNextColumn();
                ImGui::SeparatorText("Current technique placement");
                DrawPlacement(false);
                ImGui::TableNextColumn();
                DrawResult(benchmark, RunMode::Manual);
                ImGui::EndTable();
            }
            else
            {
                if (ImGui::CollapsingHeader("Current technique placement")) DrawPlacement(false);
                DrawResult(benchmark, RunMode::Manual);
            }
        }
        else if (active->Mode == RunMode::SingleGPU)
        {
            ImGui::TextWrapped("Scene GPU   %s", _primary.c_str());
            ImGui::BeginDisabled(!benchmark.CanStart());
            ImGui::BeginGroup();
            ImGui::TextDisabled("Queue placement");
            if (ImGui::Button("Run quick search")) benchmark.Launch(Action::Search);
            BistroPlot::ContinueRowIfFits(ButtonWidth("Run full sweep"));
            if (ImGui::Button("Run full sweep")) benchmark.Launch(Action::Exhaustive);
            ImGui::EndGroup();
            const float searchWidth = ButtonWidth("Find performance preset") + ButtonWidth("Find quality preset") + ImGui::GetStyle().ItemSpacing.x;
            BistroPlot::ContinueRowIfFits(searchWidth, 2.f * em);
            ImGui::BeginGroup();
            ImGui::TextDisabled("Preset search   /   %g min", BistroBenchmark::PlacementBudgetSeconds.GetValue() / 60.f);
            ProfileButtons(benchmark, Action::SingleGPU);
            ImGui::EndGroup();
            ImGui::EndDisabled();
            DrawResult(benchmark, RunMode::SingleGPU);
        }
        else if (active->Mode == RunMode::MultiGPU)
        {
            ImGui::TextWrapped("Scene GPU   %s", _primary.c_str());
            for (const auto& peer : _peers) ImGui::TextWrapped("Peer GPU    %s", peer.c_str());
            if (_peers.empty()) ImGui::TextDisabled("No peer GPU available.");
            ImGui::TextDisabled("Profile search   %g min total, up to %g min on the scene GPU",
                BistroBenchmark::PlacementBudget(_peers.size()) / 60., BistroBenchmark::PlacementBudget(0) / 60.);
            ImGui::BeginDisabled(!benchmark.CanStart() || _peers.empty());
            ProfileButtons(benchmark, Action::MultiGPU);
            ImGui::EndDisabled();
            if (ImGui::CollapsingHeader("Current technique placement")) DrawPlacement(false);
            DrawResult(benchmark, RunMode::MultiGPU);
        }
        else _compare.Draw(benchmark.ReportPath());
    }
    ImGui::EndChild();
}

void BistroInfoPanel::Draw(RenderModule& render, BistroRenderPipeline& pipeline,
    BistroBenchmark& benchmark, float frameSeconds)
{
    if (_hidden || !ImGui::GetCurrentContext()) return;
    if (std::isfinite(frameSeconds) && frameSeconds > 0)
    {
        _frameMs[_next] = frameSeconds * 1000.f;
        _next = (_next + 1) % _frameMs.size();
        _count = std::min(_count + 1, _frameMs.size());
        _elapsed += frameSeconds;
        ++_frames;
        if (_elapsed >= .25f)
        {
            _displayFps = _frames / _elapsed;
            _checkPlacement = true;
            _elapsed = 0;
            _frames = 0;
        }
    }

    if (_checkPlacement && pipeline.PresentedFrames())
    {
        _checkPlacement = false;
        RefreshPlacement(pipeline);
    }
    const bool compact = _compact;
    const auto* vp = ImGui::GetMainViewport();
    if (compact)
    {
        const float width = std::min(444.f, vp->Size.x - 16.f);
        const float height = std::min(690.f, vp->Size.y - 16.f);
        ImGui::SetNextWindowPos({vp->Pos.x + 8, vp->Pos.y + vp->Size.y - 8}, ImGuiCond_Always, {0,1});
        ImGui::SetNextWindowSizeConstraints({width, 0}, {width, height});
        ImGui::SetNextWindowSize({width, 0});
    }
    else
    {
        // Size the type for the available viewport. Whole two-pixel steps reuse baked glyphs on resize.
        const float size = std::min(vp->Size.x / 120.f, vp->Size.y / 64.f);
        ImGui::PushFont(_fullFont, std::clamp(2.f * std::round(size * .5f), 14.f, 48.f));
        const float em = ImGui::GetFontSize();
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, { .5f * em, .28f * em });
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, { .6f * em, .28f * em });
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, { .4f * em, .16f * em });
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, .95f * em);
        ImGui::PushStyleVar(ImGuiStyleVar_TabBarOverlineSize, std::max(2.f, .1f * em));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, .16f * em);
        for (const auto& [color, value] : FullPalette) ImGui::PushStyleColor(color, value);
        ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, {.5f,.5f});
        ImGui::SetNextWindowSize({vp->Size.x * .9f, vp->Size.y * .9f}, ImGuiCond_Always);
    }
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, compact ? ImVec2{14,12} :
        ImVec2{ImGui::GetFontSize(), .8f * ImGui::GetFontSize()});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, {.055f,.073f,.09f,.97f});
    if (ImGui::Begin("##BistroBenchmark", nullptr, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings))
    {
        if (compact)
        {
            Heading("Bistro benchmark");
            ImGui::SameLine();
            if (ImGui::SmallButton("Settings [F1]")) Engine::UI::OpenSettings();
            ImGui::SameLine();
            if (ImGui::SmallButton("Expand [F7]")) _compact = false;
            ImGui::Separator();
            ImGui::TextDisabled("Route idle  /  %.0f s flight", benchmark.FlightTime());
            DrawPlacement(true);
            DrawLivePlot();
        }
        else DrawFull(render, benchmark);
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    if (!compact)
    {
        ImGui::PopStyleColor(int(std::size(FullPalette)));
        ImGui::PopStyleVar(6);
        ImGui::PopFont();
    }
}

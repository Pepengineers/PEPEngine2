#include "BistroInfoPanel.h"
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
    const float width = std::min(10.f * ImGui::GetFontSize(), ImGui::GetContentRegionAvail().x);
    if (ImGui::Button("Performance", {width, 0})) benchmark.Launch(action, BistroPlacement::Performance);
    if (ImGui::GetContentRegionAvail().x >= 2 * width + ImGui::GetStyle().ItemSpacing.x) ImGui::SameLine();
    if (ImGui::Button("Quality", {width, 0})) benchmark.Launch(action, BistroPlacement::Quality);
}
}

void BistroInfoPanel::InitializeFont()
{
    auto* atlas = ImGui::GetIO().Fonts;
    if (atlas->Fonts.empty()) atlas->AddFontDefault();
    _fullFont = atlas->AddFontDefaultVector();
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
        ImGui::TableNextColumn(); ImGui::TextUnformatted(row.Technique.c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(row.Adapter.c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(row.Queue.empty() ? "-" : row.Queue.c_str());
    }
    ImGui::EndTable();
}

void BistroInfoPanel::ApplyResolution(Window& window)
{
    if (!_pendingPreset.empty())
    {
        const auto path = std::exchange(_pendingPreset, {});
        ConsoleVariablePatch patch;
        auto result = ConsoleModule::ReadPatchIni(path, patch);
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
    if (ImGui::BeginTable("Output settings", 3, ImGuiTableFlags_SizingStretchSame, {width, 0}))
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
        ImGui::TableNextColumn(); EnumSelector("Quality", quality, _displayError);
        ImGui::EndTable();
    }
    const auto& status = render.GetReconstructionStatus();
    ImGui::TextDisabled("Internal render %u x %u", common.DownscaledWidth, common.DownscaledHeight);
    if (status.Requested.Provider != status.Effective)
        ImGui::TextWrapped("Effective: %s  %s", std::string(provider.GetEnumValueName(int(status.Effective))).c_str(),
            status.FallbackReason.c_str());
    if (!_displayError.empty()) ImGui::TextUnformatted(_displayError.c_str());
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
    const bool changedTab = !_resultTab || *_resultTab != tab;
    _resultTab = tab;
    if (changedTab)
    {
        _selectedMode = tab == RunMode::SingleGPU && _compare.LatestSingleGpuIsAsync() ? RunMode::Async : tab;
        _quality = _compare.LatestQuality(ModeName(tab));
    }
    if (_latestResult != latest.Path)
    {
        _latestResult = latest.Path;
        _selectedMode = latest.Mode;
        if (latest.Mode == RunMode::SingleGPU || latest.Mode == RunMode::MultiGPU)
            _quality = latestSlot == BistroBenchmark::ResultSlot(latest.Mode, BistroPlacement::Quality);
    }
    Heading("Benchmark results");
    constexpr std::array modes{RunMode::Manual, RunMode::Async, RunMode::SingleGPU, RunMode::MultiGPU};
    for (auto mode : modes)
    {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, TraceColor(mode));
        if (ImGui::RadioButton(ModeName(mode), mode == _selectedMode) && mode != _selectedMode)
        {
            _selectedMode = mode;
            _quality = _compare.LatestQuality(ModeName(mode));
        }
        ImGui::PopStyleColor();
    }
    if (_selectedMode == RunMode::SingleGPU || _selectedMode == RunMode::MultiGPU)
    {
        if (ImGui::RadioButton("Performance##results", !_quality)) _quality = false;
        ImGui::SameLine();
        if (ImGui::RadioButton("Quality##results", _quality)) _quality = true;
        ImGui::SameLine();
    }
    if (ImGui::RadioButton("Frame interval (ms)", !_plotFps)) _plotFps = false;
    ImGui::SameLine();
    if (ImGui::RadioButton("FPS", _plotFps)) _plotFps = true;

    const auto profile = _quality ? BistroPlacement::Quality : BistroPlacement::Performance;
    const bool profiled = _selectedMode == RunMode::SingleGPU || _selectedMode == RunMode::MultiGPU;
    _compare.DrawLatest(ModeName(_selectedMode), profiled ? BistroPlacement::ProfileName[profile] : "", _plotFps,
        benchmark.CanStart(), _pendingPreset, _pendingSize);
    if (!_displayError.empty()) ImGui::TextWrapped("%s", _displayError.c_str());
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
        Heading("Bistro benchmark");
        ImGui::SameLine();
        if (ImGui::SmallButton(compact ? "Settings [F1]" : "Open settings [F1]")) Engine::UI::OpenSettings();
        ImGui::SameLine();
        if (ImGui::SmallButton(compact ? "Expand [F7]" : "Compact [F6]")) _compact = !compact;
        ImGui::Separator();
        if (compact)
        {
            ImGui::TextDisabled("Route idle  /  %.0f s flight", benchmark.FlightTime());
            DrawPlacement(true);
            DrawLivePlot();
        }
        else
        {
            bool comparing = false;
            RunMode tab = RunMode::Manual;
            if (ImGui::BeginTabBar("Benchmark modes"))
            {
                if (ImGui::BeginTabItem("Current run"))
                {
                    ImGui::BeginDisabled(!benchmark.CanStart());
                    DrawCurrentSettings(render);
                    if (ImGui::Button("Run one pass", {10.f * ImGui::GetFontSize(), 0})) benchmark.Launch(Action::PlacementRun);
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    ImGui::TextDisabled("Route %.0f s   /   Warmup %g s   /   At each painting %g s", benchmark.FlightTime(), benchmark.BaseWarmup(), benchmark.PaintingHold());
                    Heading("Technique placement");
                    DrawPlacement(false);
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Single GPU"))
                {
                    tab = RunMode::SingleGPU;
                    ImGui::Text("Scene GPU   %s", _primary.c_str());
                    ImGui::BeginDisabled(!benchmark.CanStart());
                    const float width = std::min(ImGui::GetContentRegionAvail().x, 46.f * ImGui::GetFontSize());
                    if (ImGui::BeginTable("Single GPU actions", 2, ImGuiTableFlags_SizingStretchSame, {width, 0}))
                    {
                        ImGui::TableNextColumn();
                        Heading("Queue placement");
                        const float buttonWidth = std::min(10.f * ImGui::GetFontSize(), ImGui::GetContentRegionAvail().x);
                        if (ImGui::Button("Quick search", {buttonWidth, 0})) benchmark.Launch(Action::Search);
                        if (ImGui::GetContentRegionAvail().x >= 2 * buttonWidth + ImGui::GetStyle().ItemSpacing.x) ImGui::SameLine();
                        if (ImGui::Button("Full sweep", {buttonWidth, 0})) benchmark.Launch(Action::Exhaustive);
                        ImGui::TableNextColumn();
                        ImGui::TextColored(Accent, "Profile search   %g min", BistroBenchmark::PlacementBudgetSeconds.GetValue() / 60.f);
                        ProfileButtons(benchmark, Action::SingleGPU);
                        ImGui::EndTable();
                    }
                    ImGui::EndDisabled();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Multi-GPU"))
                {
                    tab = RunMode::MultiGPU;
                    ImGui::Text("Scene GPU   %s", _primary.c_str());
                    for (const auto& peer : _peers) ImGui::Text("Peer GPU    %s", peer.c_str());
                    if (_peers.empty()) ImGui::TextDisabled("No peer GPU available.");
                    const float singleMinutes = BistroBenchmark::PlacementBudgetSeconds.GetValue() / 60.f;
                    ImGui::TextDisabled("Profile search   %g min total, up to %g min on the scene GPU", 2 * singleMinutes, singleMinutes);
                    ImGui::BeginDisabled(!benchmark.CanStart() || _peers.empty());
                    ProfileButtons(benchmark, Action::MultiGPU);
                    ImGui::EndDisabled();
                    Heading("Technique placement");
                    DrawPlacement(false);
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Compare"))
                {
                    comparing = true;
                    _compare.Draw(benchmark.ReportPath());
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
            ImGui::Separator();
            if (!benchmark.Message().empty()) ImGui::TextWrapped("%s", benchmark.Message().c_str());
            if (!comparing) DrawResult(benchmark, tab);
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    if (!compact)
    {
        ImGui::PopStyleVar(4);
        ImGui::PopFont();
    }
}

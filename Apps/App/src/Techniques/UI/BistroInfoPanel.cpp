#include "BistroInfoPanel.h"
#include "BistroPlot.h"
#include "BistroBenchmark.h"
#include "Engine.Core/BenchmarkEngine.h"
#include "Engine.UI/UI_test.h"
#include <imgui/imgui.h>
#include <shellapi.h>
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
    Heading("Profile search");
    if (ImGui::Button("Performance", {-1, 30})) benchmark.Launch(action, BistroPlacement::Performance);
    if (ImGui::Button("Quality", {-1, 30})) benchmark.Launch(action, BistroPlacement::Quality);
}
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
    for (auto [name, adapter] : pipeline.PanelOwners())
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
    if (ImGui::BeginTable("Output settings", 3, ImGuiTableFlags_SizingStretchSame))
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

void BistroInfoPanel::DrawLivePlot(float height, RunMode mode) const
{
    ImGui::Text("%.1f FPS    %.1f ms", _displayFps, _displayFps > 0 ? 1000.f / _displayFps : 0.f);
    ImGui::SameLine();
    ImGui::TextColored(TraceColor(mode), "%s", ModeName(mode));
    ImGui::PushStyleColor(ImGuiCol_PlotLines, TraceColor(mode));
    ImGui::PlotLines("##live", _frameMs.data(), int(_count), _count == _frameMs.size() ? int(_next) : 0,
        nullptr, 0.f, FLT_MAX, {-1, height});
    ImGui::PopStyleColor();
}

void BistroInfoPanel::DrawResult(const BistroBenchmark& benchmark)
{
    const auto latestSlot = benchmark.LatestResultSlot();
    const auto& latest = benchmark.LastPass(latestSlot);
    if (_latestResult != latest.Path)
    {
        _latestResult = latest.Path;
        _selectedMode = latest.Mode;
        if (latest.Mode == RunMode::SingleGPU || latest.Mode == RunMode::MultiGPU)
            _quality = latestSlot == BistroBenchmark::ResultSlot(latest.Mode, BistroPlacement::Quality);
    }
    Heading("Last completed pass");
    constexpr std::array modes{RunMode::Manual, RunMode::Async, RunMode::SingleGPU, RunMode::MultiGPU};
    for (auto mode : modes)
    {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, TraceColor(mode));
        if (ImGui::RadioButton(ModeName(mode), mode == _selectedMode)) _selectedMode = mode;
        ImGui::PopStyleColor();
    }
    if (_selectedMode == RunMode::SingleGPU || _selectedMode == RunMode::MultiGPU)
    {
        if (ImGui::RadioButton("Performance##results", !_quality)) _quality = false;
        ImGui::SameLine();
        if (ImGui::RadioButton("Quality##results", _quality)) _quality = true;
        ImGui::SameLine();
    }
    if (ImGui::RadioButton("Frame time (ms)", !_plotFps)) _plotFps = false;
    ImGui::SameLine();
    if (ImGui::RadioButton("FPS", _plotFps)) _plotFps = true;

    const auto profile = _quality ? BistroPlacement::Quality : BistroPlacement::Performance;
    const auto slot = BistroBenchmark::ResultSlot(_selectedMode, profile);
    const auto& pass = benchmark.LastPass(slot);
    if (pass.Frames.empty()) { ImGui::TextDisabled("No completed pass for this selection yet."); return; }
    auto& series = _series[slot];
    ImGui::Text("%s    %.1f FPS avg    %.1f ms avg    %.1f ms P95", pass.Name.c_str(),
        pass.Timing.Mean > 0 ? 1000 / pass.Timing.Mean : 0, pass.Timing.Mean, pass.Timing.P95);
    ImGui::SameLine();
    if (ImGui::SmallButton("Open CSV"))
        _csvError = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", pass.Path.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32
            ? "Could not open the pass CSV." : "";
    const float startTime = float(pass.Frames.front().RouteSeconds);
    const float duration = std::max(.001f, float(pass.Frames.back().RouteSeconds) - startTime);
    if (series.Path != pass.Path)
    {
        series = {};
        series.Path = pass.Path;
        // Keep both extrema per time bin, in arrival order, so brief stalls remain visible.
        const double start = pass.Frames.front().RouteSeconds;
        const double span = std::max(.001, pass.Frames.back().RouteSeconds - start);
        size_t first = 0;
        while (first < pass.Frames.size())
        {
            const int bin = int((pass.Frames[first].RouteSeconds - start) * 512 / span);
            size_t low = first, high = first, i = first + 1;
            for (; i < pass.Frames.size() && int((pass.Frames[i].RouteSeconds - start) * 512 / span) == bin; ++i)
            {
                if (pass.Frames[i].IntervalMs < pass.Frames[low].IntervalMs) low = i;
                if (pass.Frames[i].IntervalMs > pass.Frames[high].IntervalMs) high = i;
            }
            const auto point = [&](size_t k) {
                const float ms = float(pass.Frames[k].IntervalMs);
                series.Points.emplace_back(float(pass.Frames[k].RouteSeconds), ms);
                series.MaxMs = std::max(series.MaxMs, ms);
                if (ms > 0) series.MaxFps = std::max(series.MaxFps, 1000.f / ms);
            };
            point(std::min(low, high));
            if (low != high) point(std::max(low, high));
            first = i;
        }
    }
    const float maxValue = std::max(1.f, _plotFps ? series.MaxFps : series.MaxMs);
    if (!_csvError.empty()) ImGui::TextUnformatted(_csvError.c_str());
    const auto origin = ImGui::GetCursorScreenPos();
    const float width = std::max(1.f, ImGui::GetContentRegionAvail().x - 68.f);
    const float height = std::clamp(ImGui::GetContentRegionAvail().y - 30.f, 85.f, 175.f);
    const ImVec2 top{origin.x + 44, origin.y + 5};
    auto* draw = ImGui::GetWindowDrawList();
    const auto value = [&](float ms) { return _plotFps ? (ms > 0 ? 1000.f / ms : 0.f) : ms; };
    for (int i = 0; i <= 4; ++i)
    {
        const float fraction = i * .25f;
        const float y = top.y + height * (1 - fraction);
        const float x = top.x + width * fraction;
        char label[32];
        std::snprintf(label, sizeof(label), "%.0f", maxValue * fraction);
        draw->AddText({origin.x, y - 5}, ImGui::GetColorU32(ImGuiCol_TextDisabled), label);
        draw->AddLine({top.x, y}, {top.x + width, y}, ImGui::GetColorU32(ImGuiCol_Border));
        std::snprintf(label, sizeof(label), "%.1fs", startTime + duration * fraction);
        draw->AddText({x - ImGui::CalcTextSize(label).x / 2, top.y + height + 3}, ImGui::GetColorU32(ImGuiCol_TextDisabled), label);
    }
    ImVec2 previous{};
    bool first = true;
    for (const auto& point : series.Points)
    {
        const ImVec2 pos{top.x + width * (point.first - startTime) / duration, top.y + height * (1 - value(point.second) / maxValue)};
        if (!first) draw->AddLine(previous, pos, ImGui::GetColorU32(TraceColor(_selectedMode)), 1.5f);
        previous = pos;
        first = false;
    }
    ImGui::SetCursorScreenPos(top);
    ImGui::InvisibleButton("##pass cursor", {width, height});
    if (ImGui::IsItemHovered())
    {
        const float x = std::clamp(ImGui::GetMousePos().x, top.x, top.x + width);
        const double time = startTime + (x - top.x) / width * duration;
        const size_t index = BistroPlot::NearestFrame(pass.Frames, time);
        const auto& frame = pass.Frames[index];
        const ImVec2 pos{top.x + width * (float(frame.RouteSeconds) - startTime) / duration,
            top.y + height * (1 - value(float(frame.IntervalMs)) / maxValue)};
        draw->AddLine({x, top.y}, {x, top.y + height}, ImGui::GetColorU32(ImGuiCol_TextDisabled));
        draw->AddCircleFilled(pos, 4, ImGui::GetColorU32(TraceColor(_selectedMode)));
        ImGui::BeginTooltip();
        BistroPlot::DrawFrameDetails(index + 1, frame.RouteSeconds, BistroBenchmark::SectionName(frame.Section),
                                     frame.Milliseconds, frame.IntervalMs);
        ImGui::EndTooltip();
    }
    ImGui::Dummy({0, 20});
}
void BistroInfoPanel::Draw(RenderModule& render, BistroRenderPipeline& pipeline,
    BistroBenchmark& benchmark, float frameSeconds)
{
    if (_hidden || !ImGui::GetCurrentContext()) return;
    const auto settings = IConsoleVariable::ValueRevision();
    if (_checkPlacement || settings != _settingsRevision)
    {
        pipeline.CheckPanelPlacement();
        _checkPlacement = false;
        _settingsRevision = settings;
    }
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
            _elapsed = 0;
            _frames = 0;
        }
    }

    if (_snapshotRevision != pipeline.PanelRevision())
    {
        _snapshotRevision = pipeline.PanelRevision();
        RefreshPlacement(pipeline);
    }
    const bool compact = _compact;
    const auto* vp = ImGui::GetMainViewport();
    const float width = std::min(compact ? 444.f : 980.f, vp->Size.x - 16.f);
    const float height = std::min(690.f, vp->Size.y - 16.f);
    if (compact)
        ImGui::SetNextWindowPos({vp->Pos.x + 8, vp->Pos.y + vp->Size.y - 8}, ImGuiCond_Always, {0,1});
    else
        ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, {.5f,.5f});
    ImGui::SetNextWindowSizeConstraints({width,0}, {width,height});
    ImGui::SetNextWindowSize({width, compact ? 0.f : height});
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {14,12});
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
            benchmark.DrawRouteStatus(false);
            DrawPlacement(true);
            DrawLivePlot(32, benchmark.Active() ? benchmark.CurrentMode() : RunMode::Manual);
        }
        else
        {
            bool comparing = false;
            if (ImGui::BeginTabBar("Benchmark modes"))
            {
                if (ImGui::BeginTabItem("Current run"))
                {
                    ImGui::BeginDisabled(!benchmark.CanStart());
                    DrawCurrentSettings(render);
                    if (ImGui::Button("Run one pass", {160,32})) benchmark.Launch(Action::PlacementRun);
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    ImGui::TextDisabled("Route %.0f s   /   Warmup %g s   /   At each painting %g s", benchmark.FlightTime(), benchmark.BaseWarmup(), benchmark.PaintingHold());
                    Heading("Technique placement");
                    DrawPlacement(false);
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Single GPU"))
                {
                    ImGui::Text("Scene GPU   %s", _primary.c_str());
                    ImGui::BeginDisabled(!benchmark.CanStart());
                    if (ImGui::BeginTable("Single GPU actions", 2, ImGuiTableFlags_SizingStretchSame))
                    {
                        ImGui::TableNextColumn();
                        Heading("Queue placement");
                        if (ImGui::Button("Quick search", {-1,30})) benchmark.Launch(Action::Search);
                        if (ImGui::Button("Full sweep", {-1,30})) benchmark.Launch(Action::Exhaustive);
                        ImGui::TableNextColumn();
                        ProfileButtons(benchmark, Action::SingleGPU);
                        ImGui::EndTable();
                    }
                    ImGui::EndDisabled();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Multi-GPU"))
                {
                    ImGui::Text("Scene GPU   %s", _primary.c_str());
                    for (const auto& peer : _peers) ImGui::Text("Peer GPU    %s", peer.c_str());
                    if (_peers.empty()) ImGui::TextDisabled("No peer GPU available.");
                    ImGui::TextDisabled("Search budget   %g min", BistroBenchmark::PlacementBudgetSeconds.GetValue() / 60.f);
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
                    _compare.Draw(benchmark.LastPass(benchmark.LatestResultSlot()).Path);
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
            ImGui::Separator();
            if (benchmark.Active())
            {
                benchmark.DrawRouteStatus(true);
                if (ImGui::Button("Cancel run [Esc]")) benchmark.RequestCancel();
                DrawLivePlot(36, benchmark.CurrentMode());
            }
            else if (!benchmark.Message().empty()) ImGui::TextWrapped("%s", benchmark.Message().c_str());
            if (!comparing) DrawResult(benchmark);
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

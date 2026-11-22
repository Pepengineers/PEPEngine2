#include "BistroInfoPanel.h"
#include "BistroBenchmark.h"
#include "Rendering/BistroRenderPipeline.h"
#include "Engine.Core/BenchmarkEngine.h"
#include <imgui/imgui.h>
#include <algorithm>
#include <cfloat>
#include <cmath>

namespace
{
constexpr ImVec4 Accent{.93f, .76f, .49f, 1.f};

void Heading(const char* text)
{
    ImGui::TextColored(Accent, "%s", text);
}

constexpr ImVec4 Primary{.44f, .79f, .91f, 1.f};
constexpr ImVec4 Peer{.65f, .85f, .54f, 1.f};

const char* QueueName(BistroRender::Queue queue)
{
    using BistroRender::Queue;
    return queue == Queue::Direct ? "DIRECT" : queue == Queue::Compute ? "COMPUTE" : "COPY";
}

void DrawOwners(const std::map<std::string, std::string>& groups, const std::string& primary, bool compact)
{
    for (const auto& [adapter, techniques] : groups)
    {
        if (adapter == "inactive" && compact) continue;
        ImGui::TextColored(adapter == primary ? Primary : adapter == "inactive" ? Accent : Peer, "%s", adapter.c_str());
        ImGui::TextWrapped("%s", techniques.c_str());
    }
}

void DrawGraph(const BistroRenderPipeline& pipeline, int& selected)
{
    const auto& snapshot = pipeline.PanelSchedule();
    if (!snapshot) { ImGui::TextDisabled("Waiting for placement snapshot"); return; }
    const auto& packets = snapshot->Packets;
    if (packets.empty()) { ImGui::TextDisabled("Waiting for first frame"); return; }
    const auto& devices = pipeline.BenchmarkSubmissions();
    const auto& links = pipeline.FrameLinks();
    Heading("Cross-GPU dependencies");
    if (links.empty()) ImGui::TextDisabled("No cross-adapter waits or exchanges in this placement snapshot");
    else if (ImGui::BeginTable("Frame exchanges", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV))
    {
        ImGui::TableSetupColumn("Scene producer");
        ImGui::TableSetupColumn("Peer GPU");
        ImGui::TableSetupColumn("Scene consumer");
        ImGui::TableHeadersRow();
        for (const auto& link : links)
        {
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", link.Export == BistroRender::Invalid ? "External producer" : packets[link.Export].Name.c_str());
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, Peer);
            ImGui::TextWrapped("-> %s", devices.AdapterLabel(link.Owner).c_str());
            ImGui::PopStyleColor();
            ImGui::TableNextColumn();
            ImGui::TextWrapped("-> %s", link.Import == BistroRender::Invalid ? "Later frame" : packets[link.Import].Name.c_str());
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    Heading("Scene GPU pass dependencies");
    selected = std::clamp(selected, 0, int(packets.size()) - 1);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##pass", packets[selected].Name.c_str()))
    {
        for (int i = 0; i < int(packets.size()); ++i)
        {
            ImGui::PushID(i);
            if (ImGui::Selectable(packets[i].Name.c_str(), selected == i)) selected = i;
            if (selected == i) ImGui::SetItemDefaultFocus();
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    const auto origin = ImGui::GetCursorScreenPos();
    const float gap = 18.f;
    const float width = std::max(30.f, (ImGui::GetContentRegionAvail().x - 2 * gap) / 3);
    auto* draw = ImGui::GetWindowDrawList();
    const auto node = [&](int index, int column, float y)
    {
        const ImVec2 pos{origin.x + column * (width + gap), y};
        const auto& packet = packets[index];
        const float wrap = width - 12;
        const float h = ImGui::GetTextLineHeight() + ImGui::CalcTextSize(packet.Name.c_str(), nullptr, false, wrap).y + 12;
        const ImVec2 end{pos.x + width, pos.y + h};
        const auto tint = ImGui::GetColorU32(column == 1 ? Accent : Primary);
        draw->AddRectFilled(pos, end, IM_COL32(29, 43, 54, 255), 5);
        draw->AddRect(pos, end, tint, 5);
        draw->AddText({pos.x + 6, pos.y + 4}, tint, QueueName(packet.Target));
        draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(), {pos.x + 6, pos.y + 6 + ImGui::GetTextLineHeight()},
            ImGui::GetColorU32(ImGuiCol_Text), packet.Name.c_str(), nullptr, wrap);
        if (column != 1 && ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(pos, end))
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) selected = index;
        }
        return h;
    };
    const auto edge = [&](float x, float y, float targetY)
    {
        const float end = x + gap;
        const auto color = ImGui::GetColorU32(Primary);
        draw->AddBezierCubic({x, y}, {x + gap / 2, y}, {end - gap / 2, targetY}, {end - 3, targetY}, color, 1.5f);
        draw->AddTriangleFilled({end, targetY}, {end - 5, targetY - 3}, {end - 5, targetY + 3}, color);
    };
    const int focus = selected;
    const float h = node(focus, 1, origin.y);
    const float center = origin.y + h / 2;
    float left = origin.y, right = origin.y;
    for (auto dep : packets[focus].Dependencies)
    {
        const float size = node(dep, 0, left);
        edge(origin.x + width, left + size / 2, center);
        left += size + 6;
    }
    for (auto i : pipeline.PanelConsumers()[focus])
    {
        const float size = node(i, 2, right);
        edge(origin.x + 2 * width + gap, center, right + size / 2);
        right += size + 6;
    }
    ImGui::Dummy({0, std::max({origin.y + h, left, right}) - origin.y});
}

void DrawReconstruction(RenderModule& render)
{
    const auto& status = render.GetReconstructionStatus();
    const auto& common = *render.GetRenderPipelineCommonData();
    const auto console = BenchmarkEngine::GetLocator().GetModule<ConsoleModule>();
    const auto& provider = console->FindConsoleVariable(L"Render.Upscaler.Provider");
    const auto& quality = console->FindConsoleVariable(L"Render.Upscaler.Quality");
    const auto effective = provider.GetEnumValueName(static_cast<int>(status.Effective));
    const auto preset = quality.GetEnumValueName(static_cast<int>(status.Requested.Quality));
    if (status.Requested.Provider == status.Effective)
        ImGui::Text("%.*s / %.*s", int(effective.size()), effective.data(), int(preset.size()), preset.data());
    else
        ImGui::Text("%.*s (fallback)", int(effective.size()), effective.data());
    if (!status.FallbackReason.empty() && ImGui::IsItemHovered())
    {
        const auto requested = provider.GetEnumValueName(static_cast<int>(status.Requested.Provider));
        ImGui::SetTooltip("Requested: %.*s\n%s", int(requested.size()), requested.data(), status.FallbackReason.c_str());
    }
    ImGui::TextDisabled("Render %ux%u  >  Output %ux%u", common.DownscaledWidth, common.DownscaledHeight,
        common.WindowWidth, common.WindowHeight);
}
}

void BistroInfoPanel::Draw(RenderModule& render, BistroRenderPipeline& pipeline,
    BistroBenchmark& benchmark, float frameSeconds)
{
    if (_hidden || !ImGui::GetCurrentContext()) return;
    const auto settings = IConsoleVariable::ValueRevision();
    if (_checkPlacement || settings != _settingsRevision)
    {
        pipeline.CheckPanelPlacement(settings);
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

    const auto* vp = ImGui::GetMainViewport();
    const float margin = std::min(12.f, std::min(vp->Size.x, vp->Size.y) * .03f);
    const float width = std::min(_compact ? 410.f : 980.f, vp->Size.x - 2 * margin);
    const float height = vp->Size.y - 2 * margin;
    ImGui::SetNextWindowPos({vp->Pos.x + vp->Size.x - margin, vp->Pos.y + margin}, ImGuiCond_Always, {1, 0});
    ImGui::SetNextWindowSizeConstraints({width, 0}, {width, height});
    ImGui::SetNextWindowSize({width, _compact ? 0.f : std::min(680.f, height)});
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::SetNextWindowBgAlpha(.95f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {14, 12});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 7.f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, {.055f, .073f, .09f, 1.f});
    ImGui::PushStyleColor(ImGuiCol_PlotLines, Accent);
    if (ImGui::Begin("##BistroBenchmark", nullptr, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings))
    {
        Heading("BISTRO  /  BENCHMARK");
        ImGui::SameLine();
        if (ImGui::SmallButton(_compact ? "Expand" : "Minimize")) _compact = !_compact;
        ImGui::Separator();
        ImGui::Text("%.1f ms/frame    %.0f FPS", _displayFps > 0 ? 1000 / _displayFps : 0, _displayFps);
        ImGui::PlotLines("##frame time", _frameMs.data(), int(_count),
            _count == _frameMs.size() ? int(_next) : 0, nullptr, 0.f, FLT_MAX, {-1, _compact ? 28.f : 48.f});
        benchmark.DrawRouteStatus(!_compact);

        if (_snapshotRevision != pipeline.PanelRevision())
        {
            _snapshotRevision = pipeline.PanelRevision();
            _owners.clear();
            for (auto [name, adapter] : pipeline.PanelOwners())
            {
                if (name == "SceneAdapter") _primary = adapter;
                constexpr std::string_view suffix = "Adapter";
                if (name.size() >= suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
                    name.resize(name.size() - suffix.size());
                auto& list = _owners[adapter];
                if (!list.empty()) list += ", ";
                list += name;
            }
        }
        if (_compact) DrawOwners(_owners, _primary, true);
        else
        {
            ImGui::Spacing();
            if (ImGui::BeginTabBar("Benchmark views"))
            {
                if (ImGui::BeginTabItem("Overview"))
                {
                    Heading("Single GPU runs");
                    benchmark.DrawControls(false);
                    Heading("Multi-GPU runs");
                    benchmark.DrawControls(true);
                    ImGui::Separator();
                    Heading("Reconstruction");
                    DrawReconstruction(render);
                    ImGui::Separator();
                    Heading("Technique placement");
                    DrawOwners(_owners, _primary, false);
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Render graph"))
                {
                    DrawGraph(pipeline, _selectedPass);
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
        }
    }
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
}

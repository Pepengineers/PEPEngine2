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

bool Starts(std::string_view name, std::string_view prefix)
{
    return name.substr(0, prefix.size()) == prefix;
}

void DrawQueues(const BistroRenderPipeline& pipeline)
{
    constexpr const char* labels[] = {"AO", "Env", "Grid", "Particles", "Water", "Clouds", "HUD", "Posters"};
    constexpr size_t Cloud = 5;
    constexpr unsigned Direct = 1, Compute = 2, Copy = 4;
    std::array<unsigned, std::size(labels)> queues{};
    for (const auto& packet : pipeline.LastSchedule().Packets)
    {
        const std::string_view name = packet.Name;
        int i = BistroBenchmark::QueueGroup(name);
        if (name == "Game HUD raster")
            i = 6;
        else if (Starts(name, "world-space canvases"))
            i = 7;
        if (i < 0) continue;
        using BistroRender::Queue;
        queues[i] |= packet.Target == Queue::Compute ? Compute : packet.Target == Queue::Copy ? Copy : Direct;
    }
    const bool remote = pipeline.HasRemoteCloudPicture();
    for (const auto& [flag, queue] : {std::pair{Direct, "DIRECT"}, {Compute, "COMPUTE"}, {Copy, "COPY"}})
    {
        std::string row;
        for (size_t i = 0; i < queues.size(); ++i)
            if (queues[i] & flag)
            {
                if (!row.empty()) row += "  ";
                row += remote && i == Cloud ? "Cloud import" : labels[i];
            }
        if (remote && flag == Compute)
        {
            if (!row.empty()) row += "  ";
            row += "Clouds (GPU1)";
        }
        if (flag == Copy && row.empty()) continue;
        ImGui::TextDisabled("%s", queue);
        ImGui::SameLine(70);
        ImGui::TextUnformatted(row.empty() ? "-" : row.c_str());
        if (!row.empty() && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s: %s", queue, row.c_str());
    }
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

void BistroInfoPanel::Draw(RenderModule& render, const BistroRenderPipeline& pipeline,
    BistroBenchmark& benchmark, float frameSeconds)
{
    if (!ImGui::GetCurrentContext()) return;
    if (std::isfinite(frameSeconds) && frameSeconds > 0)
    {
        _fps[_next] = 1.f / frameSeconds;
        _next = (_next + 1) % _fps.size();
        _count = std::min(_count + 1, _fps.size());
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
    const float height = std::min(Height(vp->Size.x), vp->Size.y);
    const bool wide = vp->Size.x >= 1050;
    ImGuiWindowClass windowClass;
    windowClass.ClassId = ImGui::GetID("Bistro information class");
    windowClass.DockingAllowUnclassed = false;
    ImGui::SetNextWindowClass(&windowClass);
    ImGui::SetNextWindowPos({vp->Pos.x, vp->Pos.y + vp->Size.y - height});
    ImGui::SetNextWindowSize({vp->Size.x, height});
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::Begin("##BistroInformationDock", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoBringToFrontOnFocus);
    const auto dock = ImGui::GetID("Bistro information");
    ImGui::DockSpace(dock, {0, 0}, ImGuiDockNodeFlags_AutoHideTabBar | ImGuiDockNodeFlags_NoResize |
        ImGuiDockNodeFlags_NoDockingSplit | ImGuiDockNodeFlags_NoUndocking, &windowClass);
    ImGui::End();
    ImGui::PopStyleVar(2);

    ImGui::SetNextWindowClass(&windowClass);
    ImGui::SetNextWindowDockID(dock, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(.93f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {12, 8});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {8, 3});
    ImGui::PushStyleColor(ImGuiCol_WindowBg, {.064f, .072f, .084f, 1.f});
    ImGui::PushStyleColor(ImGuiCol_ChildBg, {.064f, .072f, .084f, 1.f});
    ImGui::PushStyleColor(ImGuiCol_FrameBg, {.11f, .13f, .16f, 1.f});
    ImGui::PushStyleColor(ImGuiCol_PlotLines, Accent);
    if (ImGui::Begin("Bistro information", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
    {
        const auto timing = [&]
        {
            Heading("FRAME");
            ImGui::SameLine();
            ImGui::Text("%.0f FPS  /  %.1f ms", _displayFps, _displayFps > 0 ? 1000 / _displayFps : 0);
            const float width = wide ? ImGui::GetContentRegionAvail().x : std::min(240.f, ImGui::GetContentRegionAvail().x);
            ImGui::PlotLines("##fps", _fps.data(), static_cast<int>(_count),
                _count == _fps.size() ? static_cast<int>(_next) : 0, nullptr, 0.f, FLT_MAX, {width, 18});
        };
        const auto benchmarks = [&]
        {
            Heading("SINGLE GPU");
            benchmark.DrawControls(false);
            ImGui::Separator();
            Heading("MULTI-GPU");
            benchmark.DrawControls(true);
        };
        if (wide && ImGui::BeginTable("Telemetry columns", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV))
        {
            ImGui::TableSetupColumn("Frame", ImGuiTableColumnFlags_WidthStretch, .9f);
            ImGui::TableSetupColumn("Queues", ImGuiTableColumnFlags_WidthStretch, 1.3f);
            ImGui::TableSetupColumn("Benchmark", ImGuiTableColumnFlags_WidthStretch, 1.05f);
            ImGui::TableNextColumn();
            timing();
            DrawReconstruction(render);
            ImGui::TableNextColumn();
            Heading("GPU QUEUES");
            DrawQueues(pipeline);
            ImGui::TableNextColumn();
            benchmarks();
            ImGui::EndTable();
        }
        else if (!wide)
        {
            timing();
            DrawReconstruction(render);
            ImGui::Separator();
            DrawQueues(pipeline);
            ImGui::Separator();
            benchmarks();
        }
    }
    ImGui::End();
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(2);
}

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

void DrawOwners(const std::map<std::string, std::string>& groups, const std::string& primary, bool compact)
{
    for (const auto& [adapter, techniques] : groups)
    {
        if (adapter == "inactive" && compact) continue;
        ImGui::TextColored(adapter == primary ? Primary : adapter == "inactive" ? Accent : Peer, "%s", adapter.c_str());
        ImGui::TextWrapped("%s", techniques.c_str());
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

    const auto* vp = ImGui::GetMainViewport();
    const float margin = std::min(12.f, std::min(vp->Size.x, vp->Size.y) * .03f);
    const float width = std::min(_compact ? 410.f : 980.f, vp->Size.x - 2 * margin);
    const float height = vp->Size.y - 2 * margin;
    if (_compact)
        ImGui::SetNextWindowPos({vp->Pos.x + margin, vp->Pos.y + vp->Size.y - margin}, ImGuiCond_Always, {0, 1});
    else
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
        }
    }
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
}

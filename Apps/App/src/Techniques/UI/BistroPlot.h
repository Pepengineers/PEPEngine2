#pragma once

#include <algorithm>
#include <imgui/imgui.h>

namespace BistroPlot
{
inline void ContinueRowIfFits(float width, float gap = -1.f)
{
    if (gap < 0) gap = ImGui::GetStyle().ItemSpacing.x;
    const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    if (ImGui::GetItemRectMax().x + gap + width <= right)
        ImGui::SameLine(0.f, gap);
}

inline bool Choice(const char* label, const char* first, const char* second, bool secondSelected,
    bool continueRow = false, bool inlineLabel = false)
{
    const auto& style = ImGui::GetStyle();
    const float secondWidth = ImGui::CalcTextSize(second).x + 2.f * style.FramePadding.x;
    const float choiceWidth = ImGui::CalcTextSize(first).x + 2.f * style.FramePadding.x + 1.f + secondWidth;
    const float labelWidth = ImGui::CalcTextSize(label).x;
    if (continueRow)
        ContinueRowIfFits(std::max(labelWidth, choiceWidth), 1.5f * ImGui::GetFontSize());
    ImGui::PushID(label);
    ImGui::BeginGroup();
    if (*label)
    {
        const bool shareRow = inlineLabel && labelWidth + style.ItemSpacing.x + choiceWidth <= ImGui::GetContentRegionAvail().x;
        if (shareRow) ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        if (shareRow) ImGui::SameLine();
    }
    const auto option = [&](const char* text, bool selected)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(selected ? ImGuiCol_ButtonActive : ImGuiCol_FrameBg));
        const bool pressed = ImGui::Button(text);
        ImGui::PopStyleColor();
        if (selected)
        {
            const auto min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
            const float thickness = std::max(2.f, ImGui::GetFontSize() * .1f);
            ImGui::GetWindowDrawList()->AddRectFilled({min.x, max.y - thickness}, max, ImGui::GetColorU32(ImGuiCol_CheckMark));
        }
        return pressed;
    };
    if (option(first, !secondSelected)) secondSelected = false;
    ContinueRowIfFits(secondWidth, 1.f);
    if (option(second, secondSelected)) secondSelected = true;
    ImGui::EndGroup();
    ImGui::PopID();
    return secondSelected;
}

template<class Samples>
size_t NearestFrame(const Samples& samples, double time)
{
    auto it = std::lower_bound(samples.begin(), samples.end(), time,
        [](const auto& frame, double t) { return frame.RouteSeconds < t; });
    if (it == samples.end()) return samples.size() - 1;
    if (it != samples.begin() && time - (it - 1)->RouteSeconds < it->RouteSeconds - time) --it;
    return size_t(it - samples.begin());
}

inline void DrawFrameDetails(size_t number, double time, const char* section, double frameMs, double intervalMs, bool fps)
{
    ImGui::Text("Frame #%zu   |   %.3f s", number, time);
    if (*section) ImGui::TextUnformatted(section);
    if (frameMs >= 0) ImGui::Text("Timed CPU scope   %.3f ms", frameMs);
    if (intervalMs > 0)
        ImGui::Text(fps ? "Frame rate   %.1f FPS" : "Application frame interval   %.3f ms", fps ? 1000. / intervalMs : intervalMs);
}
}

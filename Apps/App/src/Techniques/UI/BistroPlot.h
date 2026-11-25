#pragma once

#include <algorithm>
#include <imgui/imgui.h>

namespace BistroPlot
{
template<class Samples>
size_t NearestFrame(const Samples& samples, double time)
{
    auto it = std::lower_bound(samples.begin(), samples.end(), time,
        [](const auto& frame, double t) { return frame.RouteSeconds < t; });
    if (it == samples.end()) return samples.size() - 1;
    if (it != samples.begin() && time - (it - 1)->RouteSeconds < it->RouteSeconds - time) --it;
    return size_t(it - samples.begin());
}

inline void DrawFrameDetails(size_t number, double time, const char* section, double frameMs, double intervalMs)
{
    ImGui::Text("Frame #%zu   |   %.3f s", number, time);
    if (*section) ImGui::TextUnformatted(section);
    if (frameMs >= 0) ImGui::Text("Timed CPU scope   %.3f ms", frameMs);
    if (intervalMs > 0) ImGui::Text("Application frame interval   %.3f ms   (%.1f FPS)", intervalMs, 1000 / intervalMs);
}
}

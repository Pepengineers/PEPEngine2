#pragma once

#include "Common/ConsoleVariables.h"
#include "Engine.UI/OffscreenUI.h"
#include <imgui/imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace BistroRender::GameHUD
{
inline AutoConsoleVariable<bool> Enabled(L"Render.UI.Game.Enabled", true,
    L"Show the simulated game HUD in Bistro.");
inline AutoConsoleVariable<int> Quality(L"Render.UI.Game.Quality", 0,
    L"Game HUD detail level: 0, 1 or 2.", EConsoleVariableFlags::Default,
    [](int value) { return value >= 0 && value <= 2; });
inline constexpr auto Atlas = Engine::UI::UITextureId(0x42495354524f4855ull);

inline void Draw(float time)
{
    const auto display = ImGui::GetIO().DisplaySize;
    if (display.x <= 0 || display.y <= 0)
        return;
    auto& list = *ImGui::GetBackgroundDrawList();
    const auto texture = static_cast<ImTextureID>(Atlas);
    const float icon = std::clamp(display.x * .032f, 12.f, 48.f);
    const float gap = icon * .10f;
    const float width = icon * 14 + gap * 13;
    const float left = (display.x - width) * .5f;
    const float barHeight = icon * .52f;
    const float bottom = display.y - icon * .45f;
    const float barY = bottom - icon - gap - barHeight;
    const float top = barY - gap - icon;
    const ImU32 panel = IM_COL32(18, 15, 26, 218), border = IM_COL32(148, 114, 66, 240);
    list.AddRectFilled({left - gap * 2, top - gap * 2}, {left + width + gap * 2, bottom + gap * 2}, panel, 7);
    for (int skill = 0; skill < 28; ++skill)
    {
        const float x = left + (skill % 14) * (icon + gap);
        const float y = skill < 14 ? top : bottom - icon;
        const float u = float(skill % 7 * 128 + 2), v = float(skill / 7 * 128 + 2);
        const ImVec2 lo{x, y}, hi{x + icon, y + icon};
        list.AddImageRounded(texture, lo, hi, {u / 1536, v / 512},
            {(u + 124) / 1536, (v + 124) / 512}, IM_COL32_WHITE, 3);
        list.AddRect(lo, hi, border, 3);
    }

    const float health = .77f + .10f * std::sin(time * .65f);
    const float fill = left + health * width;
    const ImVec2 barStart{left, barY}, barEnd{left + width, barY + barHeight};
    list.AddRectFilled(barStart, barEnd, IM_COL32(37, 18, 30, 255), 3);
    list.AddRectFilledMultiColor(barStart, {fill, barEnd.y}, IM_COL32(112, 12, 40, 255),
        IM_COL32(218, 57, 64, 255), IM_COL32(143, 18, 46, 255), IM_COL32(63, 12, 39, 255));

    // Original MGPU-IMGUI builds start at 1–6 stars and multiply both limits by 1.2^10 or 1.2^50.
    constexpr float density[] = {1.f, 6.191736f, 9100.438f};
    const float multiplier = density[Quality.GetValue()];
    const unsigned stars = unsigned(multiplier * (1.f + 4.99f * (.5f + .5f * std::sin(time * .29f))));
    const auto alpha = std::clamp(3200 / int(std::max(stars, 1u)), 8, 210);
    const ImU32 glow = IM_COL32(255, 195, 126, alpha);
    list.PushClipRect(barStart, {fill, barEnd.y}, true);
    for (unsigned i = 0; i < stars; ++i)
    {
        unsigned random = (i + 1) * 2654435761u;
        random = (random ^ (random >> 16)) * 0x7feb352du;
        random ^= random >> 15;
        const float x = left + std::fmod(float(random & 65535) / 65536.f + time * .13f, 1.f) * (fill - left);
        const float y = barY + float(random >> 16) / 65536.f * barHeight;
        list.AddTriangleFilled({x - 2, y}, {x + 2, y - 1.4f}, {x + 1, y + 1.4f}, glow);
    }
    list.PopClipRect();
    list.AddRect(barStart, barEnd, border, 3);
    char hp[16];
    std::snprintf(hp, sizeof(hp), "HP  %d%%", int(health * 100));
    const auto text = ImGui::CalcTextSize(hp);
    list.AddText({left + (width - text.x) * .5f, barY + (barHeight - text.y) * .5f}, IM_COL32_WHITE, hp);

    const float map = std::clamp(std::min(display.x * .17f, display.y * .24f), 30.f, 224.f);
    const float inset = icon * .40f;
    const ImVec2 mapStart{display.x - map - inset, inset + std::min(90.f, display.y * .12f)};
    const ImVec2 mapEnd{mapStart.x + map, mapStart.y + map};
    list.AddRectFilled({mapStart.x - 4, mapStart.y - 4}, {mapEnd.x + 4, mapEnd.y + 4}, panel, 8);
    list.AddImageRounded(texture, mapStart, mapEnd, {1024.5f / 1536, .5f / 512},
        {1535.5f / 1536, 511.5f / 512}, IM_COL32_WHITE, 6);
    list.AddRect(mapStart, mapEnd, border, 6);
    const ImVec2 marker{mapStart.x + map * .5f, mapStart.y + map * .54f};
    list.AddTriangleFilled({marker.x, marker.y - 7}, {marker.x - 4, marker.y + 4}, {marker.x + 4, marker.y + 4},
        IM_COL32(251, 221, 147, 255));
}
}

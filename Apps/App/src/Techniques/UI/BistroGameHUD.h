#pragma once

#include "Common/ConsoleVariables.h"
#include "Engine.UI/OffscreenUI.h"
#include <imgui/imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace BistroRender::GameHUD
{
inline AutoConsoleVariable<bool> Enabled(L"Render.UI.GameHUD.Enabled", true,
    L"Show the simulated game HUD in Bistro.");
enum class HUDQuality { Low, Medium, High, Count };
inline AutoConsoleVariable<HUDQuality> Quality(L"Render.UI.GameHUD.Quality", HUDQuality::Low,
    L"Game HUD health bar particle detail.");
inline constexpr auto Atlas = Engine::UI::UITextureId(0x42495354524f4855ull);
inline constexpr auto HealthBar = Engine::UI::UITextureId(0x4249535452485042ull);

struct Bar
{
    float Width, Height, Health, Phase;
    unsigned Sparks;
};

inline Bar Draw(ImDrawList& list, float time)
{
    list._ResetForNewFrame();
    const auto display = ImGui::GetIO().DisplaySize;
    if (display.x <= 0 || display.y <= 0)
        return {};
    const auto* viewport = ImGui::GetMainViewport();
    list.Flags |= ImDrawListFlags_AllowVtxOffset;
    list.PushTexture(ImGui::GetIO().Fonts->TexRef);
    list.PushClipRect(viewport->Pos, {viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y});
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
    const ImVec2 barStart{left, barY}, barEnd{left + width, barY + barHeight};
    list.AddImageRounded(static_cast<ImTextureID>(HealthBar), barStart, barEnd, {0, 0}, {1, 1}, IM_COL32_WHITE, 3);

    const float animatedCount = 1.f + 4.99f * (.5f + .5f * std::sin(time * .29f));
    const unsigned highSparks = unsigned(9100.438f * animatedCount);
    const auto quality = Quality.GetValue();
    unsigned sparks = unsigned(animatedCount);
    if (quality == HUDQuality::Medium)
        sparks = std::max(7000u, highSparks / 2);
    else if (quality == HUDQuality::High)
        sparks = highSparks;
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
    return {width, barHeight, health, std::fmod(time * .13f, 1.f), sparks};
}
}

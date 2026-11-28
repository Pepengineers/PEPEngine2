#pragma once

#include <imgui/imgui.h>

namespace Engine::UI::PepengineTheme
{
    inline constexpr ImVec4 Ink   {0.063f, 0.106f, 0.098f, 1.f};
    inline constexpr ImVec4 Panel {0.098f, 0.169f, 0.145f, 1.f};
    inline constexpr ImVec4 Border{0.220f, 0.302f, 0.263f, 1.f};
    inline constexpr ImVec4 Paper {0.953f, 0.957f, 0.918f, 1.f};
    inline constexpr ImVec4 Mint  {0.400f, 0.859f, 0.698f, 1.f};
    inline constexpr ImVec4 Lime  {0.725f, 0.937f, 0.482f, 1.f};

    ImFontAtlasRectId LoadResourceImage(ImFontAtlas& atlas, int id);
void Initialize(ImFontAtlas& atlas, int iconResource);
    void Reset();
    ImFont* BodyFont();
    ImFont* HeadingFont();
    void Push();
    void Pop();
    void DrawBrand(float iconSize = 0.f);
    void DrawToolHeading(const char* title);
}

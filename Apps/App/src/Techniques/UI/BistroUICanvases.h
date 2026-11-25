#pragma once

#include "Engine.UI/WorldCanvas.h"
#include <array>

namespace BistroUICanvases
{
    inline constexpr Engine::UI::CanvasId Atrium{1};
    inline constexpr Engine::UI::CanvasId Terrace{2};
    inline constexpr Engine::UI::CanvasId Clouds{3};

    struct Poster
    {
        Engine::UI::CanvasId Id;
        const char* Title;
        const wchar_t* Adapter;
        const wchar_t* CrossSync;
        const wchar_t* Description;
        const char* Material;
        Engine::UI::CanvasStyle Style;
        Engine::UI::CanvasSize Size;
    };

    inline constexpr std::array Posters{
        Poster{Atrium, "NEON DISTRICT", L"Render.WorldCanvas.Atrium.Adapter", L"Render.WorldCanvas.Atrium.CrossSync", L"NEON DISTRICT",
               "BistroUINeon", Engine::UI::CanvasStyle::Neon, {768, 512}},
        Poster{Terrace, "ASTRA", L"Render.WorldCanvas.Terrace.Adapter", L"Render.WorldCanvas.Terrace.CrossSync", L"ASTRA",
               "BistroUIAstra", Engine::UI::CanvasStyle::Graffiti, {768, 512}},
        Poster{Clouds, "CLOUDS", nullptr, nullptr, L"CLOUDS",
               "BistroUIClouds", Engine::UI::CanvasStyle::Image, {1024, 512}}};
}

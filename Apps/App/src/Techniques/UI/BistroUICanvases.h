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
        Poster{Atrium, "NEON DISTRICT", L"App.UI.WorldCanvas.Adapter.Atrium", L"App.UI.WorldCanvas.CrossSync.Atrium", L"NEON DISTRICT",
               "BistroUINeon", Engine::UI::CanvasStyle::Neon, {768, 512}},
        Poster{Terrace, "ASTRA", L"App.UI.WorldCanvas.Adapter.Terrace", L"App.UI.WorldCanvas.CrossSync.Terrace", L"ASTRA",
               "BistroUIAstra", Engine::UI::CanvasStyle::Graffiti, {768, 512}},
        Poster{Clouds, "CLOUDS", L"App.UI.WorldCanvas.Adapter.Clouds", L"App.UI.WorldCanvas.CrossSync.Clouds", L"CLOUDS",
               "BistroUIClouds", Engine::UI::CanvasStyle::Image, {1024, 512}}};
}

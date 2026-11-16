#pragma once

#include "Engine.UI/WorldCanvas.h"
#include <array>

namespace BistroUICanvases
{
    inline constexpr Engine::UI::CanvasId Atrium{1};
    inline constexpr Engine::UI::CanvasId Terrace{2};
    inline constexpr Engine::UI::CanvasId Clouds{3};

    class Posters
    {
    public:
        std::array<std::shared_ptr<const Engine::UI::CanvasFrame>, 3> BuildFrames(double timeSeconds)
        {
            const auto quality = Engine::UI::GetCanvasQuality();
            return {_atrium.BuildFrame({768, 512}, timeSeconds, quality),
                _terrace.BuildFrame({768, 512}, timeSeconds, quality),
                _clouds.BuildFrame({1024, 512}, timeSeconds, quality)};
        }

    private:
        Engine::UI::WorldCanvas _atrium{Atrium, "NEON DISTRICT", Engine::UI::CanvasStyle::Neon};
        Engine::UI::WorldCanvas _terrace{Terrace, "ASTRA", Engine::UI::CanvasStyle::Graffiti};
        Engine::UI::WorldCanvas _clouds{Clouds, "CLOUDS", Engine::UI::CanvasStyle::Image};
    };
}

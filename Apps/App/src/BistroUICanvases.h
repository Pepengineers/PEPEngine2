#pragma once

#include "Engine.UI/WorldCanvas.h"
#include <array>

namespace BistroUICanvases
{
    inline constexpr Engine::UI::CanvasId Atrium{1};
    inline constexpr Engine::UI::CanvasId Terrace{2};

    class Posters
    {
    public:
        std::array<std::shared_ptr<const Engine::UI::CanvasFrame>, 2> BuildFrames(double timeSeconds)
        {
            const auto quality = Engine::UI::GetCanvasQuality();
            return {_atrium.BuildFrame({384, 216}, timeSeconds, quality),
                _terrace.BuildFrame({480, 270}, timeSeconds + 3.0, quality)};
        }

    private:
        Engine::UI::WorldCanvas _atrium{Atrium, "ATRIUM"};
        Engine::UI::WorldCanvas _terrace{Terrace, "TERRACE"};
    };
}

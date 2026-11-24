#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <vector>

namespace BistroRender
{
    struct ReflectionStamp
    {
        uint64_t Version = 0;
        std::chrono::steady_clock::time_point Captured;
    };

    struct ReflectionFrame
    {
        struct Probe
        {
            uint64_t Identity = 0;
            unsigned DueFaces = 0;
            std::array<ReflectionStamp, 6> Used{};
        };
        uint64_t Version = 0;
        std::vector<Probe> Probes;
    };
}

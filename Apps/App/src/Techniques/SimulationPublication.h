#pragma once

#include "../Graph/RenderGraph.h"
#include <string_view>

namespace BistroRender
{
    struct SimulationPublication
    {
        std::string_view Role;
        Id Owner = 0;
        size_t Lane = 0;
        uint64_t Generation = 0, Version = 0;
        double SimulatedSeconds = 0;
    };
}

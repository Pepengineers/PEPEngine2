#pragma once

#include "BistroPlacementSearch.h"
#include "../Resources/Resource.h"

namespace BistroRun
{
struct Map
{
    const char* Id;
    const char* Name;
    const char* Scene;
    const char* Route;
    int Background;
    decltype(BistroPlacement::Techniques)& Techniques;
};

inline constexpr Map Maps[]{
    {"bistro", "Bistro", "Scenes/Amazon Lumberyard Bistro", "bistro-flight-v1", IDR_BISTRO_BACKGROUND,
        BistroPlacement::Techniques}};

enum class Mode { Manual, Profile, Automatic };
struct Launch
{
    const Map* Level = &Maps[0];
    BistroPlacement::Size Output{1280, 800};
    Mode Kind = Mode::Manual;
    BistroPlacement::Profile Profile = BistroPlacement::Performance;
    bool AllowUpscalers = false;
    ConsoleVariablePatch Settings;
};
}

#pragma once

#include "Common/ConsoleVariables.h"
#include "Engine.RendererDX12/GDX12ReflectionSchedule.h"
#include <array>

namespace BistroReflections
{
    inline AutoConsoleVariable<bool> UseQualityPreset(L"Render.ReflectionProbes.UseQualityPreset", false,
        L"Let reflection quality select the Bistro probe count, face size and prefilter samples");
    inline constexpr std::array<const wchar_t*, 4> AdapterNames{
        L"Render.ReflectionProbes.First.Adapter", L"Render.ReflectionProbes.Second.Adapter",
        L"Render.ReflectionProbes.Third.Adapter", L"Render.ReflectionProbes.Fourth.Adapter"};
    inline AutoConsoleVariable<int> FirstAdapter(AdapterNames[0], -1,
        L"DXGI hardware index for the first Bistro probe; -1 uses GPU0", EConsoleVariableFlags::Default,
        [](int value) { return value >= -1; });
    inline AutoConsoleVariable<int> SecondAdapter(AdapterNames[1], -1,
        L"DXGI hardware index for the second Bistro probe; -1 uses GPU0", EConsoleVariableFlags::Default,
        [](int value) { return value >= -1; });
    inline AutoConsoleVariable<int> ThirdAdapter(AdapterNames[2], -1,
        L"DXGI hardware index for the third Bistro probe; -1 uses GPU0", EConsoleVariableFlags::Default,
        [](int value) { return value >= -1; });
    inline AutoConsoleVariable<int> FourthAdapter(AdapterNames[3], -1,
        L"DXGI hardware index for the fourth Bistro probe; -1 uses GPU0", EConsoleVariableFlags::Default,
        [](int value) { return value >= -1; });
    inline const std::array Adapters{&FirstAdapter, &SecondAdapter, &ThirdAdapter, &FourthAdapter};

    inline constexpr unsigned Side(unsigned quality) { return quality ? 32u << quality : 0; }
    inline constexpr unsigned Samples(unsigned quality) { return quality ? 16u << quality : 0; }
}

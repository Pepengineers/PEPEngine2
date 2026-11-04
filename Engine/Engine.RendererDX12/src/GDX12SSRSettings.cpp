#include "Engine.RendererDX12/GDX12SSRSettings.h"
#include "Common/ConsoleVariables.h"
#include <algorithm>

namespace
{
    static AutoConsoleVariable<bool> Enabled(L"Render.SSR.Enabled", true,
        L"Enable AMD SSSR and Reflections Denoiser.", EConsoleVariableFlags::Default);
    static AutoConsoleVariable<int> Quality(L"Render.SSR.Quality", 2,
        L"0 Off, 1 Low, 2 Mid, 3 High. Manual overrides survive preset changes.",
        EConsoleVariableFlags::Default, [](const int v) { return v >= 0 && v <= 3; });
    static AutoConsoleVariable<int> MaxIntersections(L"Render.SSR.MaxIntersections", -1,
        L"Depth hierarchy lookups per ray: -1 preset, 1..1024.",
        EConsoleVariableFlags::Default, [](const int v) { return v == -1 || (v >= 1 && v <= 1024); });
    static AutoConsoleVariable<int> MinOccupancy(L"Render.SSR.MinOccupancy", -1,
        L"Stop traversal below this active lane count: -1 preset, 0..32.",
        EConsoleVariableFlags::Default, [](const int v) { return v >= -1 && v <= 32; });
    static AutoConsoleVariable<int> MostDetailedMip(L"Render.SSR.MostDetailedMip", -1,
        L"Finest depth mip for rough rays: -1 preset, 0..6 (clamped to image mip count).",
        EConsoleVariableFlags::Default, [](const int v) { return v >= -1 && v <= 6; });
    static AutoConsoleVariable<int> SamplesPerQuad(L"Render.SSR.SamplesPerQuad", -1,
        L"Minimum rays per 2x2 quad: -1 preset, 1, 2 or 4.",
        EConsoleVariableFlags::Default, [](const int v) { return v == -1 || v == 1 || v == 2 || v == 4; });
    static AutoConsoleVariable<bool> VarianceGuided(L"Render.SSR.VarianceGuided", true,
        L"Trace extra rays where temporal luminance variance exceeds VarianceThreshold.", EConsoleVariableFlags::Default);
    static AutoConsoleVariable<float> RoughnessCutoff(L"Render.SSR.RoughnessCutoff", -1.0f,
        L"Perceptual roughness cutoff: -1 preset or 0..1. Higher roughness keeps global IBL.",
        EConsoleVariableFlags::Default, [](const float v) { return std::isfinite(v) && (v == -1 || (v >= 0 && v <= 1)); });
    static AutoConsoleVariable<float> Thickness(L"Render.SSR.Thickness", 8.0f,
        L"View-space hit tolerance in scene centimetres: (0,1000]. Default 8 cm.",
        EConsoleVariableFlags::Default, [](const float v) { return std::isfinite(v) && v > 0 && v <= 1000; });
    static AutoConsoleVariable<float> TemporalStability(L"Render.SSR.TemporalStability", -1.0f,
        L"Denoiser history stability: -1 preset or 0..1.",
        EConsoleVariableFlags::Default, [](const float v) { return std::isfinite(v) && (v == -1 || (v >= 0 && v <= 1)); });
    static AutoConsoleVariable<float> VarianceThreshold(L"Render.SSR.VarianceThreshold", 0.05f,
        L"Temporal luminance variance threshold: 0..1, default 0.05.",
        EConsoleVariableFlags::Default, [](const float v) { return std::isfinite(v) && v >= 0 && v <= 1; });
}

GDX12SSRSettings GetSSRSettings()
{
    const int quality = Quality.GetValue();
    const int preset = (std::max)(quality, 1) - 1;
    const int intersections[] = { 64, 128, 256 };
    const int occupancy[] = { 8, 4, 0 };
    const int mip[] = { 2, 1, 0 };
    const int samples[] = { 1, 2, 4 };
    const float cutoff[] = { 0.5f, 0.7f, 0.9f };
    const float stability[] = { 0.8f, 0.9f, 0.95f };
    const auto inherit = [](auto value, auto defaultValue) { return value == -1 ? defaultValue : value; };
    return { Enabled.GetValue() && quality != 0,
        inherit(MaxIntersections.GetValue(), intersections[preset]),
        inherit(MinOccupancy.GetValue(), occupancy[preset]),
        inherit(MostDetailedMip.GetValue(), mip[preset]),
        inherit(SamplesPerQuad.GetValue(), samples[preset]),
        VarianceGuided.GetValue(), inherit(RoughnessCutoff.GetValue(), cutoff[preset]),
        Thickness.GetValue(), inherit(TemporalStability.GetValue(), stability[preset]), VarianceThreshold.GetValue() };
}

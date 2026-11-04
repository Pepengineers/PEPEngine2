#include "Engine.RendererDX12/GDX12AmbientOcclusion.h"
#include "Common/ConsoleVariables.h"
#include <algorithm>
#include <cmath>

namespace
{
    constexpr auto Runtime = EConsoleVariableFlags::Default;
    static AutoConsoleVariable<int> ExecutionPoint(L"Render.AO.ExecutionPoint", 1, L"0 after depth prepass, 1 before lighting", Runtime,
        [](const int v) { return v == 0 || v == 1; });
    static AutoConsoleVariable<int> Enabled(L"Render.AO.Enabled", 1, L"AO enabled: 0/1", Runtime,
        [](const int v) { return v == 0 || v == 1; });
    static AutoConsoleVariable<int> Quality(L"Render.AO.Quality", 2, L"0 Off, 1 Low, 2 Mid, 3 High", Runtime,
        [](const int v) { return v >= 0 && v <= 3; });
    static AutoConsoleVariable<int> Implementation(L"Render.AO.Implementation", 2, L"0 SSAO, 1 HBAO, 2 XeGTAO, 3 AMD CACAO", Runtime,
        [](const int v) { return v >= 0 && v <= 3; });
    static AutoConsoleVariable<int> Samples(L"Render.AO.SSAO.Samples", -1, L"Hemisphere samples: -1 preset, 4..64", Runtime,
        [](const int v) { return v == -1 || (v >= 4 && v <= 64); });
    static AutoConsoleVariable<int> Directions(L"Render.AO.HBAO.Directions", -1, L"Azimuth directions: -1 preset, 2..16", Runtime,
        [](const int v) { return v == -1 || (v >= 2 && v <= 16); });
    static AutoConsoleVariable<int> Steps(L"Render.AO.HBAO.Steps", -1, L"Horizon steps per direction: -1 preset, 2..16", Runtime,
        [](const int v) { return v == -1 || (v >= 2 && v <= 16); });
    static AutoConsoleVariable<int> Denoise(L"Render.AO.DenoisePasses", -1, L"-1 preset, 0..8 edge-aware passes", Runtime,
        [](const int v) { return v >= -1 && v <= 8; });
    static AutoConsoleVariable<int> GTAOSlices(L"Render.AO.XeGTAO.Slices", -1, L"-1 preset, 1..8 integration slices", Runtime,
        [](const int v) { return v == -1 || (v >= 1 && v <= 8); });
    static AutoConsoleVariable<int> GTAOSteps(L"Render.AO.XeGTAO.Steps", -1, L"-1 preset, 1..16 steps per half slice", Runtime,
        [](const int v) { return v == -1 || (v >= 1 && v <= 16); });
    static AutoConsoleVariable<int> CACAOQuality(L"Render.AO.CACAO.Quality", -1, L"-1 preset, native quality 0..4 (4 adaptive)", Runtime,
        [](const int v) { return v >= -1 && v <= 4; });
    static AutoConsoleVariable<float> Radius(L"Render.AO.RadiusCm", 50.0f, L"World radius in cm: 0.1..10000", Runtime,
        [](const float v) { return std::isfinite(v) && v >= 0.1f && v <= 10000; });
    static AutoConsoleVariable<float> Intensity(L"Render.AO.Intensity", 1.0f, L"Occlusion multiplier: 0..5", Runtime,
        [](const float v) { return std::isfinite(v) && v >= 0 && v <= 5; });
    static AutoConsoleVariable<float> Bias(L"Render.AO.SSAO.BiasCm", 0.5f, L"Hemisphere depth bias in cm: 0..100", Runtime,
        [](const float v) { return std::isfinite(v) && v >= 0 && v <= 100; });
    static AutoConsoleVariable<float> HorizonBias(L"Render.AO.HorizonBias", 0.06f, L"HBAO/CACAO horizon sine threshold: 0..0.2", Runtime,
        [](const float v) { return std::isfinite(v) && v >= 0 && v <= 0.2f; });
    static AutoConsoleVariable<float> FadeStart(L"Render.AO.FadeStartCm", 5000.0f, L"Fade start distance in cm: 0..1000000", Runtime,
        [](const float v) { return std::isfinite(v) && v >= 0 && v <= 1000000; });
    static AutoConsoleVariable<float> FadeEnd(L"Render.AO.FadeEndCm", 15000.0f, L"Fade end in cm: 1..1000001; resolved >= start+1", Runtime,
        [](const float v) { return std::isfinite(v) && v >= 1 && v <= 1000001; });
    static AutoConsoleVariable<float> Sharpness(L"Render.AO.Sharpness", 0.98f, L"Depth edge preservation: 0..1", Runtime,
        [](const float v) { return std::isfinite(v) && v >= 0 && v <= 1; });
    static AutoConsoleVariable<float> GTAOFalloff(L"Render.AO.XeGTAO.Falloff", 0.615f, L"Radius falloff fraction: 0.05..1", Runtime,
        [](const float v) { return std::isfinite(v) && v >= 0.05f && v <= 1; });
    static AutoConsoleVariable<float> GTAOThin(L"Render.AO.XeGTAO.ThinOccluder", 0.0f, L"Thin occluder compensation: 0..0.7", Runtime,
        [](const float v) { return std::isfinite(v) && v >= 0 && v <= 0.7f; });
    static AutoConsoleVariable<float> CACAOPower(L"Render.AO.CACAO.Power", 1.5f, L"Native shadow exponent: 0.5..5", Runtime,
        [](const float v) { return std::isfinite(v) && v >= 0.5f && v <= 5; });
    static AutoConsoleVariable<float> CACAOClamp(L"Render.AO.CACAO.Clamp", 0.98f, L"Native maximum occlusion: 0..1", Runtime,
        [](const float v) { return std::isfinite(v) && v >= 0 && v <= 1; });
    static AutoConsoleVariable<float> CACAODetail(L"Render.AO.CACAO.Detail", 0.5f, L"Native detail shadow strength: 0..5", Runtime,
        [](const float v) { return std::isfinite(v) && v >= 0 && v <= 5; });
    static AutoConsoleVariable<float> CACAOAdaptive(L"Render.AO.CACAO.AdaptiveLimit", 0.45f, L"Native adaptive sample budget: 0..1", Runtime,
        [](const float v) { return std::isfinite(v) && v >= 0 && v <= 1; });

    int Override(const int value, const int preset) { return value < 0 ? preset : value; }
}

GDX12AOSettings GetAmbientOcclusionSettings()
{
    GDX12AOSettings s = {};
    s.Enabled = Enabled.GetValue();
    s.Quality = Quality.GetValue();
    s.Implementation = Implementation.GetValue();
    const int level = std::max(1, s.Quality) - 1;
    constexpr int samples[] = { 8, 16, 32 };
    constexpr int directions[] = { 4, 6, 8 };
    constexpr int steps[] = { 4, 6, 8 };
    constexpr int slices[] = { 1, 2, 3 };
    constexpr int gtaoSteps[] = { 2, 2, 3 };
    constexpr int cacaoQuality[] = { 1, 2, 4 };
    s.Samples = Override(Samples.GetValue(), samples[level]);
    s.Directions = Override(Directions.GetValue(), directions[level]);
    s.Steps = Override(Steps.GetValue(), steps[level]);
    s.DenoisePasses = Override(Denoise.GetValue(), level + 1);
    s.GTAOSlices = Override(GTAOSlices.GetValue(), slices[level]);
    s.GTAOSteps = Override(GTAOSteps.GetValue(), gtaoSteps[level]);
    s.CACAOQuality = Override(CACAOQuality.GetValue(), cacaoQuality[level]);
    s.RadiusCm = Radius.GetValue();
    s.Intensity = Intensity.GetValue();
    s.BiasCm = Bias.GetValue();
    s.HorizonBias = HorizonBias.GetValue();
    s.FadeStartCm = FadeStart.GetValue();
    s.FadeEndCm = std::max(FadeEnd.GetValue(), s.FadeStartCm + 1);
    s.Sharpness = Sharpness.GetValue();
    s.GTAOFalloff = GTAOFalloff.GetValue();
    s.GTAOThinOccluder = GTAOThin.GetValue();
    s.CACAOPower = CACAOPower.GetValue();
    s.CACAOClamp = CACAOClamp.GetValue();
    s.CACAODetail = CACAODetail.GetValue();
    s.CACAOAdaptiveLimit = CACAOAdaptive.GetValue();
    return s;
}

int GetAmbientOcclusionExecutionPoint() { return ExecutionPoint.GetValue(); }

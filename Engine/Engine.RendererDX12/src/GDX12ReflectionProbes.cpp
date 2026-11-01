#include "Engine.RendererDX12/GDX12ReflectionProbes.h"
#include "Common/ConsoleVariables.h"
#include <cmath>

static AutoConsoleVariable<bool> Enabled(L"Render.Reflection.Enabled", true,
    L"Enable local reflection probes. Default true.", EConsoleVariableFlags::Default);
static AutoConsoleVariable<int> Quality(L"Render.Reflection.Quality", 2,
    L"0 Off, 1 Low, 2 Mid (default), 3 High.", EConsoleVariableFlags::Default,
    [](int value) { return value >= 0 && value <= 3; });
static AutoConsoleVariable<int> Implementation(L"Render.Reflection.Implementation", 1,
    L"0 Dynamic, 1 Baked static color+depth with dynamic overlay (default).", EConsoleVariableFlags::Default,
    [](int value) { return value == 0 || value == 1; });
static AutoConsoleVariable<UINT> Resolution(L"Render.Reflection.Resolution", 0,
    L"Face width in pixels; 0 inherits quality (64/128/256). Power of two, 16..512.", EConsoleVariableFlags::Default,
    [](UINT value) { return value == 0 || (value >= 16 && value <= 512 && (value & (value - 1)) == 0); });
static AutoConsoleVariable<UINT> Faces(L"Render.Reflection.FacesPerFrame", 0,
    L"Capture faces per frame; 0 inherits quality (1/2/6), override 1..6.", EConsoleVariableFlags::Default,
    [](UINT value) { return value <= 6; });
static AutoConsoleVariable<UINT> Samples(L"Render.Reflection.PrefilterSamples", 0,
    L"GGX samples per texel; 0 inherits quality (32/64/128), override 8..1024.", EConsoleVariableFlags::Default,
    [](UINT value) { return value == 0 || (value >= 8 && value <= 1024); });
static AutoConsoleVariable<UINT> Interval(L"Render.Reflection.UpdateInterval", 0,
    L"Minimum frames between complete probe generations. 0 uses each probe author value, override 1..3600.",
    EConsoleVariableFlags::Default, [](UINT value) { return value <= 3600; });

GDX12ReflectionSettings GetReflectionSettings()
{
    GDX12ReflectionSettings result;
    result.Quality = Quality.GetValue();
    result.Enabled = Enabled.GetValue() && result.Quality != 0;
    result.Implementation = Implementation.GetValue();
    const UINT level = static_cast<UINT>((std::max)(1, result.Quality) - 1);
    const UINT resolution = Resolution.GetValue();
    const UINT faces = Faces.GetValue();
    const UINT samples = Samples.GetValue();
    result.Resolution = resolution ? resolution : 64u << level;
    result.FacesPerFrame = faces ? faces : (level == 2 ? 6u : level + 1);
    result.PrefilterSamples = samples ? samples : 32u << level;
    result.UpdateInterval = Interval.GetValue();
    return result;
}

bool IsValidReflectionProbe(const GDX12ReflectionProbe& probe)
{
    const auto finite = [](Vector3 value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    };
    if (!finite(probe.Position) || !finite(probe.BoxMin) || !finite(probe.BoxMax) ||
        !finite(probe.BoxMax - probe.BoxMin) ||
        probe.BoxMin.x >= probe.BoxMax.x || probe.BoxMin.y >= probe.BoxMax.y || probe.BoxMin.z >= probe.BoxMax.z ||
        probe.Position.x < probe.BoxMin.x || probe.Position.x > probe.BoxMax.x ||
        probe.Position.y < probe.BoxMin.y || probe.Position.y > probe.BoxMax.y ||
        probe.Position.z < probe.BoxMin.z || probe.Position.z > probe.BoxMax.z) return false;
    return std::isfinite(probe.BlendDistance) && probe.BlendDistance > 0 &&
        std::isfinite(probe.Intensity) && probe.Intensity >= 0 && probe.Intensity <= 16 &&
        std::isfinite(probe.NearPlane) && probe.NearPlane >= 0.01f &&
        std::isfinite(probe.FarPlane) && probe.FarPlane > probe.NearPlane && probe.FarPlane <= 1000000 &&
        probe.UpdateInterval >= 1 && probe.UpdateInterval <= 3600;
}

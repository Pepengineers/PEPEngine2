#include "Engine.RendererDX12/GDX12ReflectionProbes.h"
#include "Common/ConsoleVariables.h"
#include "Engine.RendererDX12/GDX12Quality.h"
#include <cmath>
#include <stdexcept>

static AutoConsoleVariable<bool> Enabled(L"Render.Reflection.Enabled", true,
    L"Enable local reflection probes. Default true.", EConsoleVariableFlags::Default);
static AutoConsoleVariable<GDX12Quality> Quality(L"Render.Reflection.Quality", GDX12Quality::Mid,
    L"0 Off, 1 Low, 2 Mid (default), 3 High, 4 Epic.", EConsoleVariableFlags::Default);
GDX12ReflectionSettings GetReflectionSettings()
{
    GDX12ReflectionSettings result;
    result.Quality = Quality.GetInt();
    result.Enabled = Enabled.GetValue() && result.Quality != 0;
    return result;
}

GDX12ReflectionCaptureSettings ResolveReflectionCaptureSettings(const GDX12ReflectionCaptureSettings& author, const int quality)
{
    if (quality < 0 || quality > 4) throw std::invalid_argument("Invalid reflection quality");
    const UINT level = static_cast<UINT>((std::max)(1, quality) - 1);
    auto result = author;
    if (!result.Resolution) result.Resolution = 64u << level;
    if (!result.FacesPerFrame) result.FacesPerFrame = level >= 2 ? 6u : level + 1;
    if (!result.PrefilterSamples) result.PrefilterSamples = 32u << level;
    return result;
}

bool IsValidReflectionProbe(const GDX12ReflectionProbe& probe)
{
    const auto finite = [](const Vector3 value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    };
    if (!finite(probe.Position) || !finite(probe.BoxMin) || !finite(probe.BoxMax) ||
        !finite(probe.BoxMax - probe.BoxMin) ||
        probe.BoxMin.x >= probe.BoxMax.x || probe.BoxMin.y >= probe.BoxMax.y || probe.BoxMin.z >= probe.BoxMax.z ||
        probe.Position.x < probe.BoxMin.x || probe.Position.x > probe.BoxMax.x ||
        probe.Position.y < probe.BoxMin.y || probe.Position.y > probe.BoxMax.y ||
        probe.Position.z < probe.BoxMin.z || probe.Position.z > probe.BoxMax.z) return false;
    const bool validResolution = probe.Resolution == 0 || (probe.Resolution >= 16 && probe.Resolution <= 512 &&
        (probe.Resolution & (probe.Resolution - 1)) == 0);
    return probe.Implementation >= 0 && probe.Implementation <= 2 && validResolution &&
        probe.FacesPerFrame <= 6 && (probe.PrefilterSamples == 0 ||
            (probe.PrefilterSamples >= 8 && probe.PrefilterSamples <= 1024)) &&
        std::isfinite(probe.BlendDistance) && probe.BlendDistance > 0 &&
        std::isfinite(probe.Intensity) && probe.Intensity >= 0 && probe.Intensity <= 16 &&
        std::isfinite(probe.NearPlane) && probe.NearPlane >= 0.01f &&
        std::isfinite(probe.FarPlane) && probe.FarPlane > probe.NearPlane && probe.FarPlane <= 1000000 &&
        probe.UpdateInterval >= 1 && probe.UpdateInterval <= 3600;
}

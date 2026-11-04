#include "Engine.RendererDX12/GDX12GrassWind.h"
#include "Common/ConsoleVariables.h"
#include "Engine.RendererDX12/GDX12Quality.h"
#include <algorithm>
#include <cmath>

namespace
{
    static AutoConsoleVariable<bool> GrassEnabled(L"Render.Grass.Enabled", true, L"Enable grass components", EConsoleVariableFlags::Default);
    static AutoConsoleVariable<GDX12Quality> GrassQuality(L"Render.Grass.Quality", GDX12Quality::Mid, L"Grass: 0 Off, 1 Low, 2 Mid, 3 High", EConsoleVariableFlags::Default);
    static AutoConsoleVariable<bool> WindEnabled(L"Render.Wind.Enabled", true, L"Enable wind components", EConsoleVariableFlags::Default);
    static AutoConsoleVariable<GDX12Quality> WindQuality(L"Render.Wind.Quality", GDX12Quality::Mid, L"Wind: 0 Off, 1 Low, 2 Mid, 3 High", EConsoleVariableFlags::Default);
    bool Range(const float value, const float low, const float high) { return std::isfinite(value) && value >= low && value <= high; }
    bool Point(const XMFLOAT3& v) { return Range(v.x, -1e7f, 1e7f) && Range(v.y, -1e7f, 1e7f) && Range(v.z, -1e7f, 1e7f); }
}

GDX12GrassWindControls GetGrassWindControls()
{
    return { GrassEnabled.GetValue(), GrassQuality.GetInt(), WindEnabled.GetValue(), WindQuality.GetInt() };
}

GDX12GrassSettings ResolveGrassSettings(const GDX12GrassSettings& author, const int quality)
{
    auto settings = author;
    const int q = (std::clamp)(quality, 1, 3) - 1;
    const UINT budgets[] = { 16384, 65536, 262144 };
    const UINT segments[] = { 1, 3, 5 };
    if (!settings.MaxBlades) settings.MaxBlades = budgets[q];
    if (!settings.Segments) settings.Segments = segments[q];
    return settings;
}
GDX12WindSettings ResolveWindSettings(const GDX12WindSettings& author, const int quality)
{
    auto settings = author;
    const int q = (std::clamp)(quality, 1, 3) - 1;
    const UINT grids[] = { 32, 64, 128 };
    const UINT iterations[] = { 8, 20, 40 };
    if (!settings.GridResolution) settings.GridResolution = grids[q];
    if (!settings.PressureIterations) settings.PressureIterations = iterations[q];
    return settings;
}

bool IsValidGrassSettings(const GDX12GrassSettings& s)
{
    if (s.Implementation != GDX12GrassImplementation::Ribbon && s.Implementation != GDX12GrassImplementation::CrossedRibbons) return false;
    if (!Range(s.ExtentsCm.x, 1, 1e6f) || !Range(s.ExtentsCm.y, 1, 1e6f) ||
        !Range(s.DensityPerSquareMetre, 0, 100000) || s.MaxBlades > 262144 || s.Segments > 8 ||
        !Range(s.HeightCm, 0.1f, 10000) || !Range(s.HeightVariation, 0, 1) || !Range(s.WidthCm, 0.01f, 1000) ||
        !Range(s.BendSeconds, 0, 10) || !Range(s.MaxBendCm, 0, 10000) || !Range(s.LodDistanceCm, 1, 1e7f) ||
        !Range(s.FarCardDistanceCm, s.LodDistanceCm, 1e7f) || !Range(s.MaxDistanceCm, s.FarCardDistanceCm, 1e7f) ||
        s.RootsCm.size() > 262144) return false;
    for (const auto& root : s.RootsCm) if (!Point(root)) return false;
    return true;
}

bool IsValidWindSettings(const GDX12WindSettings& s)
{
    if (s.Implementation != GDX12WindImplementation::Analytic && s.Implementation != GDX12WindImplementation::ProjectedGrid) return false;
    return Range(s.ExtentsCm.x, 1, 1e6f) && Range(s.ExtentsCm.y, 1, 1e6f) &&
        Range(s.Direction.x, -1, 1) && Range(s.Direction.y, -1, 1) && s.Direction.LengthSquared() > 1e-8f &&
        Range(s.SpeedCmPerSecond, 0, s.MaxSpeedCmPerSecond) && Range(s.GustAmplitude, 0, 1) &&
        Range(s.GustFrequencyHz, 0, 100) && Range(s.GustWavelengthCm, 1, 1e6f) &&
        Range(s.ForcePerSecond, 0, 100) && Range(s.DampingPerSecond, 0, 100) &&
        Range(s.MaxSpeedCmPerSecond, 0.01f, 10000) && Range(s.TimeScale, 0, 10) &&
        Range(s.MaxStepSeconds, 0.0001f, 0.1f) && s.MaxSubsteps >= 1 && s.MaxSubsteps <= 16 &&
        (s.GridResolution == 0 || (s.GridResolution >= 8 && s.GridResolution <= 256)) && s.PressureIterations <= 128;
}

UINT GrassBladeCount(const GDX12GrassSettings& s)
{
    const double count = s.RootsCm.empty() ? std::ceil(4.0 * s.ExtentsCm.x * s.ExtentsCm.y * 0.0001 * s.DensityPerSquareMetre) :
        static_cast<double>(s.RootsCm.size());
    return static_cast<UINT>((std::min)(count, static_cast<double>(s.MaxBlades)));
}
BoundingBox GrassLocalBounds(const GDX12GrassSettings& s)
{
    Vector3 low(-s.ExtentsCm.x, 0, -s.ExtentsCm.y), high(s.ExtentsCm.x, 0, s.ExtentsCm.y);
    if (!s.RootsCm.empty())
    {
        low = high = Vector3(s.RootsCm.front());
        for (const auto& root : s.RootsCm)
        {
            low = Vector3::Min(low, Vector3(root));
            high = Vector3::Max(high, Vector3(root));
        }
    }
    const float bend = s.MaxBendCm + s.WidthCm;
    low -= Vector3(bend, bend, bend);
    high += Vector3(bend, s.HeightCm * (1 + s.HeightVariation) + bend, bend);
    BoundingBox result;
    BoundingBox::CreateFromPoints(result, low, high);
    return result;
}

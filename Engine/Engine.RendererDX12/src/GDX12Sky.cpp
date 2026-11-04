#include "Engine.RendererDX12/GDX12Sky.h"
#include "Common/ConsoleVariables.h"
#include <cmath>
#include <stdexcept>

static AutoConsoleVariable<bool> SkyBoxEnabled(L"Render.SkyBox.Enabled", true,
    L"Enable the selected SkyBox component", EConsoleVariableFlags::Default);
static AutoConsoleVariable<UINT> SkyBoxQuality(L"Render.SkyBox.Quality", 3,
    L"SkyBox: 0 Off, 1 Low, 2 Mid, 3 High", EConsoleVariableFlags::Default,
    [](const UINT value) { return value <= 3; });
static AutoConsoleVariable<bool> AtmosphereEnabled(L"Render.SkyAtmosphere.Enabled", true,
    L"Enable the selected SkyAtmosphere component", EConsoleVariableFlags::Default);
static AutoConsoleVariable<UINT> AtmosphereQuality(L"Render.SkyAtmosphere.Quality", 2,
    L"Atmosphere: 0 Off, 1 Low, 2 Mid, 3 High", EConsoleVariableFlags::Default,
    [](const UINT value) { return value <= 3; });

UINT GetSkyBoxQuality() { return SkyBoxEnabled.GetValue() ? SkyBoxQuality.GetValue() : 0; }
UINT GetSkyAtmosphereQuality() { return AtmosphereEnabled.GetValue() ? AtmosphereQuality.GetValue() : 0; }

namespace
{
    void Range(const float value, const float minimum, const float maximum)
    {
        if (!std::isfinite(value) || value < minimum || value > maximum)
            throw std::invalid_argument("Sky component parameter outside its documented finite range");
    }
    void Range(const Vector3& value, const float minimum, const float maximum)
    {
        Range(value.x, minimum, maximum);
        Range(value.y, minimum, maximum);
        Range(value.z, minimum, maximum);
    }
    void Budget(const UINT value, const UINT minimum, const UINT maximum)
    {
        if (value && (value < minimum || value > maximum))
            throw std::invalid_argument("Sky LUT/sample budget outside its documented range");
    }
    void ValidateIBL(const SkyIBLSettings& settings)
    {
        Budget(settings.CubeResolution, 16, 1024);
        Budget(settings.FilterSamples, 16, 1024);
        Range(settings.Intensity, 0.0f, 100.0f);
    }
}

void ValidateSkySettings(const SkyBoxSettings& settings)
{
    if (settings.Cubemap.empty()) throw std::invalid_argument("SkyBox requires a DDS cubemap asset");
    Range(settings.RadianceMultiplier, 0.0f, 100.0f);
    Range(settings.RotationRadians, -10000.0f, 10000.0f);
    ValidateIBL(settings.IBL);
}

void ValidateSkySettings(const SkyAtmosphereSettings& s)
{
    Range(s.PlanetCenterCm, -1.0e11f, 1.0e11f);
    Range(s.PlanetRadiusKm, 100.0f, 100000.0f);
    Range(s.AtmosphereHeightKm, 1.0f, 1000.0f);
    Range(s.RayleighScatteringPerKm, 0.0f, 10.0f);
    Range(s.MieScatteringPerKm, 0.0f, 10.0f);
    Range(s.MieAbsorptionPerKm, 0.0f, 10.0f);
    Range(s.OzoneAbsorptionPerKm, 0.0f, 10.0f);
    Range(s.RayleighScaleHeightKm, 0.01f, 1000.0f);
    Range(s.MieScaleHeightKm, 0.01f, 1000.0f);
    Range(s.MieAnisotropy, -0.95f, 0.95f);
    Range(s.OzoneCenterKm, 0.0f, s.AtmosphereHeightKm);
    Range(s.OzoneHalfWidthKm, 0.01f, 1000.0f);
    Range(s.GroundAlbedo, 0.0f, 0.99f);
    Range(s.DirectionToSun, -1.0e6f, 1.0e6f);
    if (s.DirectionToSun.LengthSquared() < 1.0e-12f)
        throw std::invalid_argument("SkyAtmosphere sun direction must be nonzero");
    Range(s.SunIlluminanceLux, 0.0f, 200000.0f);
    Range(s.SunAngularRadiusRadians, 0.001f, 0.1f);
    Range(s.LuminanceScale, 0.0000001f, 1.0f);
    // Keep the unattenuated disk representable in the existing RGBA16F HDR chain.
    const float diskSolidAngle = XM_PI * std::pow(std::sin(s.SunAngularRadiusRadians), 2.0f);
    Range(s.SunIlluminanceLux * (s.LuminanceScale / diskSolidAngle), 0.0f, 60000.0f);
    Range(s.IBLCapturePositionCm, -1.0e11f, 1.0e11f);
    if ((s.IBLCapturePositionCm - s.PlanetCenterCm).Length() * 0.00001f < s.PlanetRadiusKm)
        throw std::invalid_argument("Atmosphere IBL capture position is inside the planet");
    Range(s.AerialDistanceKm, 0.01f, 1000.0f);
    ValidateIBL(s.IBL);
    Budget(s.TransmittanceWidth, 16, 512);
    Budget(s.TransmittanceHeight, 8, 128);
    Budget(s.MultipleScatteringSize, 8, 64);
    Budget(s.SkyViewWidth, 32, 1024);
    Budget(s.SkyViewHeight, 16, 512);
    Budget(s.AerialWidth, 8, 64);
    Budget(s.AerialHeight, 8, 64);
    Budget(s.AerialDepth, 8, 64);
    Budget(s.TransmittanceSamples, 8, 256);
    Budget(s.SkySamples, 8, 256);
    Budget(s.MultipleScatteringDirections, 8, 128);
    Budget(s.MultipleScatteringSamples, 8, 128);
    Budget(s.AerialSamples, 4, 128);
}

SkyIBLSettings ResolveSkyIBLSettings(const SkyIBLSettings& source, const UINT quality)
{
    if (quality < 1 || quality > 3) throw std::invalid_argument("Sky quality must be 1..3 when resolving budgets");
    auto result = source;
    if (!result.CubeResolution) result.CubeResolution = 128u << (quality - 1);
    if (!result.FilterSamples) result.FilterSamples = 32u << (quality - 1);
    return result;
}

SkyAtmosphereSettings ResolveSkyAtmosphereSettings(const SkyAtmosphereSettings& source, const UINT quality)
{
    ValidateSkySettings(source);
    auto s = source;
    s.IBL = ResolveSkyIBLSettings(source.IBL, quality);
    const auto inherit = [](UINT& value, const UINT preset) { if (!value) value = preset; };
    inherit(s.TransmittanceWidth, 128u << (quality - 1));
    inherit(s.TransmittanceHeight, 32u << (quality - 1));
    inherit(s.MultipleScatteringSize, quality == 1 ? 16 : 32);
    inherit(s.SkyViewWidth, 128u << (quality - 1));
    inherit(s.SkyViewHeight, 64u << (quality - 1));
    inherit(s.AerialWidth, quality == 1 ? 16 : 32);
    inherit(s.AerialHeight, quality == 1 ? 16 : 32);
    inherit(s.AerialDepth, 16u << (quality - 1));
    inherit(s.TransmittanceSamples, 32u << (quality - 1));
    inherit(s.SkySamples, 16u << (quality - 1));
    inherit(s.MultipleScatteringDirections, 16u << (quality - 1));
    inherit(s.MultipleScatteringSamples, quality == 1 ? 16 : quality == 2 ? 20 : 32);
    inherit(s.AerialSamples, 8 * quality);
    s.DirectionToSun.Normalize();
    return s;
}

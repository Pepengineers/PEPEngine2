#include "Engine.RendererDX12/GDX12DepthOfField.h"
#include "Common/ConsoleVariables.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace
{
    static AutoConsoleVariable<bool> Enabled(L"Render.DOF.Enabled", true, L"Enable FidelityFX DoF");
    static AutoConsoleVariable<int> Quality(L"Render.DOF.Quality", 2, L"0 Off, 1 Low, 2 Mid, 3 High",
        EConsoleVariableFlags::Default, [](int v) { return v >= 0 && v <= 3; });
    static AutoConsoleVariable<float> Focus(L"Render.DOF.FocusDistanceCm", 500.0f, L"Focus distance in cm [1, 1000000]",
        EConsoleVariableFlags::Default, [](float v) { return v >= 1.0f && v <= 1000000.0f; });
    static AutoConsoleVariable<float> Aperture(L"Render.DOF.FNumber", 2.8f, L"Lens f-number [0.7, 64]",
        EConsoleVariableFlags::Default, [](float v) { return v >= 0.7f && v <= 64.0f; });
    static AutoConsoleVariable<float> Sensor(L"Render.DOF.SensorHeightMm", 24.0f, L"Vertical sensor size in mm [1, 100]",
        EConsoleVariableFlags::Default, [](float v) { return v >= 1.0f && v <= 100.0f; });
    static AutoConsoleVariable<float> CocLimit(L"Render.DOF.CocLimit", 0.0f, L"Max full-res CoC radius / image height [0, 0.25]; 0 preset",
        EConsoleVariableFlags::Default, [](float v) { return v >= 0.0f && v <= 0.25f; });
    static AutoConsoleVariable<int> Rings(L"Render.DOF.Rings", 0, L"Maximum kernel rings [1, 12]; 0 preset",
        EConsoleVariableFlags::Default, [](int v) { return v >= 0 && v <= 12; });
    static AutoConsoleVariable<bool> RingMerge(L"Render.DOF.RingMerge", true, L"Merge outer kernel rings using the bilateral mip chain");
}

GDX12DepthOfFieldSettings GetDepthOfFieldSettings()
{
    const int quality = Quality.GetValue();
    const unsigned rings[] = { 0, 3, 5, 8 };
    const float limits[] = { 0.0f, 0.025f, 0.05f, 0.1f };
    const int overrideRings = Rings.GetValue();
    const float overrideLimit = CocLimit.GetValue();
    return { Enabled.GetValue() && quality != 0, quality, Focus.GetValue(), Aperture.GetValue(), Sensor.GetValue(),
        overrideLimit > 0.0f ? overrideLimit : limits[quality],
        overrideRings ? static_cast<unsigned>(overrideRings) : rings[quality], RingMerge.GetValue() };
}

GDX12CircleOfConfusion CalculateDepthOfFieldCoc(const GDX12DepthOfFieldSettings& settings,
    float verticalFovRadians, unsigned height, float projectionZZ, float projectionZW, float projectionWZ)
{
    if (!std::isfinite(verticalFovRadians) || verticalFovRadians < 0.0174532925f || verticalFovRadians > 3.12413936f ||
        !height || !std::isfinite(settings.FocusDistanceCm) || settings.FocusDistanceCm <= 0.0f ||
        !std::isfinite(settings.FNumber) || settings.FNumber <= 0.0f ||
        !std::isfinite(settings.SensorHeightMm) || settings.SensorHeightMm <= 0.0f ||
        !std::isfinite(projectionZZ) || !std::isfinite(projectionZW) || !std::isfinite(projectionWZ) ||
        projectionZZ < 0.0f || projectionZW >= 0.0f || projectionWZ <= 0.0f)
        throw std::invalid_argument("DoF requires a finite RH reverse-Z perspective camera");
    const float sensorCm = settings.SensorHeightMm * 0.1f;
    const float focalLengthCm = sensorCm / (2.0f * std::tan(verticalFovRadians * 0.5f));
    // A physical focus plane must lie beyond the focal length, even at extreme FOVs.
    const float focusCm = (std::max)(settings.FocusDistanceCm, focalLengthCm + 0.01f);
    const float apertureRadiusCm = focalLengthCm / (2.0f * settings.FNumber);
    const float halfResPixelsPerCm = 0.5f * height / sensorCm;
    const float factor = halfResPixelsPerCm * apertureRadiusCm * focalLengthCm / (focusCm - focalLengthCm);
    return { factor * -focusCm * projectionZW / projectionWZ, factor * (focusCm * projectionZZ / projectionWZ - 1.0f) };
}

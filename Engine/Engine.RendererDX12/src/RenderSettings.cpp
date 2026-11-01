#include "Engine.RendererDX12/RenderSettings.h"
#include "Common/ConsoleVariables.h"

namespace
{
    using namespace RenderSettings;
    bool ValidQuality(Quality value) { return value >= Quality::Off && value <= Quality::High; }
    bool ValidAO(AOImplementation value) { return value >= AOImplementation::SSAO && value <= AOImplementation::XeGTAO; }
    bool Radius(float value) { return value > 0 && value <= 100000; }
    bool Intensity(float value) { return value >= 0 && value <= 8; }
    bool Bias(float value) { return value >= 0 && value <= 1; }
    bool Samples(int value) { return value == -1 || (value >= 4 && value <= 64); }
    bool Denoise(int value) { return value >= -1 && value <= 4; }
    bool Shutter(float value) { return value >= 0 && value <= 360; }
    bool BlurRadius(float value) { return value >= 1 && value <= 256; }
    constexpr auto Runtime = EConsoleVariableFlags::Default;

    static AutoConsoleVariable<bool> upscalerEnabled(L"Render.Upscaler.Enabled", true, L"Enable XeSS; false renders at native resolution");
    static AutoConsoleVariable<Quality> upscalerQuality(L"Render.Upscaler.Quality", Quality::High,
        L"0 native, 1 XeSS Performance, 2 Balanced, 3 Ultra Quality Plus", Runtime, ValidQuality);

    static AutoConsoleVariable<bool> aoEnabled(L"Render.AO.Enabled", true, L"Enable ambient occlusion");
    static AutoConsoleVariable<Quality> aoQuality(L"Render.AO.Quality", Quality::Mid,
        L"0 Off, 1 Low, 2 Mid, 3 High", Runtime, ValidQuality);
    static AutoConsoleVariable<AOImplementation> aoImplementation(L"Render.AO.Implementation", AOImplementation::XeGTAO,
        L"0 SSAO, 1 HBAO, 2 XeGTAO", Runtime, ValidAO);
    static AutoConsoleVariable<float> aoRadius(L"Render.AO.Radius", 100.0f, L"AO radius in world units (Bistro: centimetres)", Runtime, Radius);
    static AutoConsoleVariable<float> aoIntensity(L"Render.AO.Intensity", 1.0f, L"AO strength, 0 neutral", Runtime, Intensity);
    static AutoConsoleVariable<float> aoBias(L"Render.AO.Bias", 0.02f, L"Horizon bias", Runtime, Bias);
    static AutoConsoleVariable<int> aoSamples(L"Render.AO.Samples", -1, L"-1 preset, otherwise 4..64", Runtime, Samples);
    static AutoConsoleVariable<int> aoDenoise(L"Render.AO.DenoisePasses", -1, L"-1 preset, otherwise 0..4", Runtime, Denoise);

    static AutoConsoleVariable<bool> mbEnabled(L"Render.MotionBlur.Enabled", false, L"Enable motion blur");
    static AutoConsoleVariable<Quality> mbQuality(L"Render.MotionBlur.Quality", Quality::Mid,
        L"0 Off, 1 Low, 2 Mid, 3 High", Runtime, ValidQuality);
    static AutoConsoleVariable<float> mbShutter(L"Render.MotionBlur.ShutterAngle", 180.0f,
        L"Exposure as a shutter angle in degrees, 0 preserves the image", Runtime, Shutter);
    static AutoConsoleVariable<float> mbRadius(L"Render.MotionBlur.MaxRadius", 32.0f, L"Maximum blur radius in pixels", Runtime, BlurRadius);
    static AutoConsoleVariable<int> mbSamples(L"Render.MotionBlur.Samples", -1, L"-1 preset, otherwise 4..64", Runtime, Samples);

    unsigned SampleCount(Quality level, int overrideValue)
    {
        constexpr unsigned presets[] = { 0, 8, 16, 32 };
        return overrideValue < 0 ? presets[static_cast<int>(level)] : static_cast<unsigned>(overrideValue);
    }
}

namespace RenderSettings
{
    Upscaler GetUpscaler()
    {
        const auto quality = upscalerQuality.GetValue();
        return { upscalerEnabled.GetValue() && quality != Quality::Off, quality };
    }
    AO GetAO()
    {
        const auto quality = aoQuality.GetValue();
        const auto denoise = aoDenoise.GetValue();
        return { aoEnabled.GetValue() && quality != Quality::Off, quality, aoImplementation.GetValue(),
            aoRadius.GetValue(), aoIntensity.GetValue(), aoBias.GetValue(),
            SampleCount(quality, aoSamples.GetValue()),
            denoise < 0 ? (quality == Quality::High ? 2u : 1u) : static_cast<unsigned>(denoise) };
    }
    MotionBlur GetMotionBlur()
    {
        const auto quality = mbQuality.GetValue();
        const float shutter = mbShutter.GetValue() / 360.0f;
        return { mbEnabled.GetValue() && quality != Quality::Off && shutter > 0,
            quality, shutter, mbRadius.GetValue(), SampleCount(quality, mbSamples.GetValue()) };
    }
}

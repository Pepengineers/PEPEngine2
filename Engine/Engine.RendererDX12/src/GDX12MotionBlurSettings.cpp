#include "Engine.RendererDX12/GDX12MotionBlurSettings.h"
#include "Common/ConsoleVariables.h"
#include <cmath>

namespace
{
    static AutoConsoleVariable<bool> Enabled(L"Render.MotionBlur.Enabled", true,
        L"Enable feature-aware motion blur.");
    static AutoConsoleVariable<int> Quality(L"Render.MotionBlur.Quality", 2,
        L"0=Off, 1=Low (19 samples), 2=Mid (27), 3=High (35).",
        EConsoleVariableFlags::Default, [](const int value) { return value >= 0 && value <= 3; });
    static AutoConsoleVariable<float> Shutter(L"Render.MotionBlur.ShutterFraction", 0.5f,
        L"Exposure/frame duration, 0..1; 0.5 = 180 degrees, centered exposure.",
        EConsoleVariableFlags::Default,
        [](const float value) { return std::isfinite(value) && value >= 0.0f && value <= 1.0f; });
    static AutoConsoleVariable<float> Radius(L"Render.MotionBlur.MaxRadius", 40.0f,
        L"Maximum half-exposure radius in output pixels, 1..128.",
        EConsoleVariableFlags::Default,
        [](const float value) { return std::isfinite(value) && value >= 1.0f && value <= 128.0f; });
    static AutoConsoleVariable<int> Samples(L"Render.MotionBlur.Samples", 0,
        L"Gather taps, 4..128; 0 inherits quality. Manual override survives quality changes.",
        EConsoleVariableFlags::Default,
        [](const int value) { return value == 0 || (value >= 4 && value <= 128); });
    static AutoConsoleVariable<float> CenterWeight(L"Render.MotionBlur.CenterWeight", 40.0f,
        L"Paper k, 1..100; center contribution is samples/(k*radius). Smaller preserves thin features more strongly.",
        EConsoleVariableFlags::Default,
        [](const float value) { return std::isfinite(value) && value >= 1.0f && value <= 100.0f; });
    static AutoConsoleVariable<float> VelocityThreshold(L"Render.MotionBlur.VelocityThreshold", 1.5f,
        L"Paper gamma, 0.1..8 input pixels beyond the 0.5 pixel floor; transition to local motion direction.",
        EConsoleVariableFlags::Default,
        [](const float value) { return std::isfinite(value) && value >= 0.1f && value <= 8.0f; });
}

GDX12MotionBlurSettings GetMotionBlurSettings()
{
    const int quality = Quality.GetValue();
    const int samples = Samples.GetValue();
    constexpr unsigned presetSamples[] = { 0, 19, 27, 35 };
    const float shutter = Shutter.GetValue();
    return { Enabled.GetValue() && quality != 0 && shutter > 0.0f,
        samples == 0 ? presetSamples[quality] : static_cast<unsigned>(samples),
        shutter, Radius.GetValue(), CenterWeight.GetValue(), VelocityThreshold.GetValue() };
}

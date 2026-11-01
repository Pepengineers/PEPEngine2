#pragma once
#include <cstdint>

namespace RenderSettings
{
    enum class Quality : int { Off, Low, Mid, High };
    enum class AOImplementation : int { SSAO, HBAO, XeGTAO };

    struct Upscaler
    {
        bool Enabled;
        Quality Level;
    };
    Upscaler GetUpscaler();

    struct AO
    {
        bool Enabled;
        Quality Level;
        AOImplementation Implementation;
        float Radius, Intensity, Bias;
        unsigned Samples, DenoisePasses;
    };
    AO GetAO();

    struct MotionBlur
    {
        bool Enabled;
        Quality Level;
        float ShutterFraction, MaxRadius;
        unsigned Samples;
    };
    MotionBlur GetMotionBlur();
}

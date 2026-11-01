#pragma once

struct GDX12MotionBlurSettings
{
    bool Enabled;
    unsigned Samples;
    float ShutterFraction;
    float MaxRadius;
    float CenterWeight;
    float VelocityThreshold;
};

GDX12MotionBlurSettings GetMotionBlurSettings();

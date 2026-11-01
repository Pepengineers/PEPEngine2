#pragma once

struct GDX12DepthOfFieldSettings
{
    bool Enabled;
    int Quality;
    float FocusDistanceCm;
    float FNumber;
    float SensorHeightMm;
    float CocLimit;
    unsigned Rings;
    bool RingMerge;
};

struct GDX12CircleOfConfusion
{
    float Scale;
    float Bias;
};

GDX12DepthOfFieldSettings GetDepthOfFieldSettings();
// CoC is a signed radius in half-resolution pixels: positive near, negative far.
GDX12CircleOfConfusion CalculateDepthOfFieldCoc(const GDX12DepthOfFieldSettings& settings,
    float verticalFovRadians, unsigned height, float projectionZZ, float projectionZW, float projectionWZ);

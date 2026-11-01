#pragma once

struct GDX12AOSettings
{
    int Enabled;
    int Quality;
    int Implementation;
    int Samples;
    int Directions;
    int Steps;
    int DenoisePasses;
    int GTAOSlices;
    int GTAOSteps;
    int CACAOQuality;
    float RadiusCm;
    float Intensity;
    float BiasCm;
    float HorizonBias;
    float FadeStartCm;
    float FadeEndCm;
    float Sharpness;
    float GTAOFalloff;
    float GTAOThinOccluder;
    float CACAOPower;
    float CACAOClamp;
    float CACAODetail;
    float CACAOAdaptiveLimit;

    bool IsActive() const { return Enabled && Quality != 0 && Intensity > 0; }
};

// One validated snapshot per frame; resolving presets never writes back to CVars.
GDX12AOSettings GetAmbientOcclusionSettings();

// 0 after depth prepass, 1 before deferred lighting. Snapshotted by RenderModule.
int GetAmbientOcclusionExecutionPoint();

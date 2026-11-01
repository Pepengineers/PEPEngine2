#pragma once

struct GDX12SSRSettings
{
    bool Enabled;
    int MaxIntersections;
    int MinOccupancy;
    int MostDetailedMip;
    int SamplesPerQuad;
    bool VarianceGuided;
    float RoughnessCutoff;
    float Thickness;
    float TemporalStability;
    float VarianceThreshold;
};

GDX12SSRSettings GetSSRSettings();

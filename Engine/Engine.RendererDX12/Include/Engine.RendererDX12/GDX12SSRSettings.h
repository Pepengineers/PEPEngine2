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

    bool operator!=(const GDX12SSRSettings& other) const
    {
        return Enabled != other.Enabled || MaxIntersections != other.MaxIntersections ||
            MinOccupancy != other.MinOccupancy || MostDetailedMip != other.MostDetailedMip ||
            SamplesPerQuad != other.SamplesPerQuad || VarianceGuided != other.VarianceGuided ||
            RoughnessCutoff != other.RoughnessCutoff || Thickness != other.Thickness ||
            TemporalStability != other.TemporalStability || VarianceThreshold != other.VarianceThreshold;
    }
};

GDX12SSRSettings GetSSRSettings();

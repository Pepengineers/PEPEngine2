#pragma once
#include "Engine.RendererDX12/D3DHelpers.h"
#include <vector>
#include <cstdint>

enum class GDX12GrassImplementation : std::uint8_t { Ribbon, CrossedRibbons };
enum class GDX12WindImplementation : std::uint8_t { Analytic, ProjectedGrid };

struct GDX12GrassSettings
{
    GDX12GrassImplementation Implementation = GDX12GrassImplementation::Ribbon;
    Vector2 ExtentsCm = Vector2(500, 500);
    std::vector<XMFLOAT3> RootsCm;
    float DensityPerSquareMetre = 0;
    UINT MaxBlades = 0;
    UINT Segments = 0;
    UINT Seed = 1;
    float HeightCm = 40;
    float HeightVariation = 0.25f;
    float WidthCm = 3;
    float BendSeconds = 0.15f;
    float MaxBendCm = 30;
    float LodDistanceCm = 1500;
    float FarCardDistanceCm = 3000;
    float MaxDistanceCm = 6000;
    bool CastShadows = true;
    bool CaptureReflections = true;
};

struct GDX12WindSettings
{
    GDX12WindImplementation Implementation = GDX12WindImplementation::ProjectedGrid;
    Vector2 ExtentsCm = Vector2(1000, 1000);
    Vector2 Direction = Vector2(1, 0);
    float SpeedCmPerSecond = 100;
    float GustAmplitude = 0.5f;
    float GustFrequencyHz = 0.5f;
    float GustWavelengthCm = 500;
    float ForcePerSecond = 2;
    float DampingPerSecond = 0.2f;
    float MaxSpeedCmPerSecond = 500;
    float TimeScale = 1;
    float MaxStepSeconds = 1.0f / 60.0f;
    UINT MaxSubsteps = 4;
    UINT GridResolution = 0;
    UINT PressureIterations = 0;
    UINT ResetRevision = 0;
};

struct GDX12GrassWindControls
{
    bool GrassEnabled = true;
    int GrassQuality = 2;
    bool WindEnabled = true;
    int WindQuality = 2;
};
GDX12GrassWindControls GetGrassWindControls();
GDX12GrassSettings ResolveGrassSettings(const GDX12GrassSettings& author, int quality);
GDX12WindSettings ResolveWindSettings(const GDX12WindSettings& author, int quality);
bool IsValidGrassSettings(const GDX12GrassSettings& settings);
bool IsValidWindSettings(const GDX12WindSettings& settings);
UINT GrassBladeCount(const GDX12GrassSettings& settings);
BoundingBox GrassLocalBounds(const GDX12GrassSettings& settings);

struct GDX12GrassInstance : GDX12GrassSettings
{
    uint64_t Id = 0;
    uint64_t WindId = 0;
    Matrix World = Matrix::Identity;
    UINT MaterialIndex = 0;
    uint64_t MaterialRevision = 0;
    UINT ResetRevision = 0;
};
struct GDX12WindInstance : GDX12WindSettings
{
    uint64_t Id = 0;
    Vector2 CenterCm = Vector2::Zero;
    float DeltaSeconds = 0;
};

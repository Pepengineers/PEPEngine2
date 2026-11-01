#pragma once
#include "Engine.RendererDX12/D3DHelpers.h"
#include "Engine.RendererDX12/RenderSettings.h"
#include <vector>

struct GDX12GrassSettings
{
    Vector2 Extents = {100,100};
    float ChunkSize = 100;
    unsigned TuftsPerChunk = 128;
    float Height = 50, Width = 3, Bend = 1;
    float LodNear = 1000, LodFar = 3000, MaxDistance = 6000;
    uint32_t Seed = 1;
    uint64_t WindEntity = 0, ResetRevision = 0;
};
struct GDX12WindSettings
{
    float HalfExtent = 2000, Force = 120, DirectionDegrees = 0, Dissipation = .2f;
    unsigned GridResolution = 0, PressureIterations = 0;
    uint64_t ResetRevision = 0;
};
struct GDX12GrassDesc
{
    uint64_t Entity = 0;
    GDX12GrassSettings Settings;
    Matrix World = Matrix::Identity;
    UINT TransformIndex = 0, MaterialIndex = 0;
    std::vector<UINT> Instances;
};
struct GDX12WindDesc
{
    uint64_t Entity = 0;
    GDX12WindSettings Settings;
    Vector3 Center = Vector3::Zero;
};
struct GDX12GrassWindDesc
{
    uint64_t World = 0;
    float DeltaTime = 0;
    std::vector<GDX12GrassDesc> Grass;
    std::vector<GDX12WindDesc> Winds;
};
namespace RenderSettings
{
    struct Grass { bool Enabled; Quality Level; float DensityScale, HeightScale, BendScale, LodScale; unsigned Blades, Segments; };
    struct Wind { bool Enabled; Quality Level; float ForceScale, DirectionOffset; int GridResolution, PressureIterations; };
    Grass GetGrass();
    Wind GetWind();
}
void ValidateGrass(const GDX12GrassSettings& settings);
void ValidateWind(const GDX12WindSettings& settings);
unsigned GrassChunkCount(const GDX12GrassSettings& settings);

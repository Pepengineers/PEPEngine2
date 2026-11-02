#pragma once
#include "Engine.RendererDX12/GDX12ConstantStructures.h"
#include "Engine.Core/Jobs/JobSystem.h"
#include <vector>

struct GDX12LightGridSettings
{
    UINT TileSize, DepthSlices;
    float FarDepthCm;
};
GDX12LightGridSettings GetLightGridSettings();

struct GDX12LightGridConstants
{
    XMUINT4 Size = {}; // Columns, rows, depth slices, mask words per cell.
    XMFLOAT4 Depth = {}; // log2(Z) scale/bias, XY cell size, near plane.
};
static_assert(sizeof(GDX12LightGridConstants) == 32);

struct GDX12LightGrid
{
    UINT DepthSlice(float z) const;
    GDX12LightGridConstants Constants;
    std::vector<UINT> Masks;
};

class GDX12LightGridBuildSystem final : public ParallelJob
{
public:
    // Synchronous boundary: all slice jobs finish before the caller uploads the grid.
    void Build(GDX12LightGrid& grid, const Matrix& projection, const Matrix& view,
        float nearPlane, float farPlane, UINT width, UINT height,
        const std::vector<GDX12LocalLightConstants>& lights, JobSystem* jobs = nullptr,
        GDX12LightGridSettings settings = GetLightGridSettings());
private:
    void Execute(size_t slice) override;
    struct Sphere
    {
        Vector3 Center;
        float Radius, Lo, Hi, Margin;
        UINT First, Last, Id;
    };
    GDX12LightGrid* _grid = nullptr;
    Matrix _projection;
    UINT _width = 0, _height = 0;
    std::vector<float> _z;
    std::vector<Sphere> _spheres;
};

#include "Engine.RendererDX12/GDX12LightGrid.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Common/ConsoleVariables.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace
{
    AutoConsoleVariable<UINT> TileSize(L"Render.Lighting.Cluster.TileSize", 64,
        L"Frustum light grid XY cell size in pixels; restart required.", EConsoleVariableFlags::ReadOnly,
        [](const UINT value) { return value >= 8 && value <= 1024; });
    AutoConsoleVariable<UINT> DepthSlices(L"Render.Lighting.Cluster.DepthSlices", 32,
        L"Frustum light grid logarithmic depth slices; restart required.", EConsoleVariableFlags::ReadOnly,
        [](const UINT value) { return value >= 1 && value <= 128; });
    AutoConsoleVariable<float> FarDepth(L"Render.Lighting.Cluster.FarDepthCm", 100000.0f,
        L"Logarithmic grid depth in cm; the last slice also covers farther visible geometry. Restart required.",
        EConsoleVariableFlags::ReadOnly,
        [](const float value) { return std::isfinite(value) && value >= 1 && value <= 1e9f; });
}

GDX12LightGridSettings GetLightGridSettings()
{
    return { TileSize.GetValue(), DepthSlices.GetValue(), FarDepth.GetValue() };
}

UINT GDX12LightGrid::DepthSlice(const float z) const
{
    return static_cast<UINT>((std::clamp)(std::log2((std::max)(z, Constants.Depth.w)) * Constants.Depth.x + Constants.Depth.y,
        0.0f, float(Constants.Size.z - 1)));
}

void GDX12LightGridBuildSystem::Build(GDX12LightGrid& grid, const Matrix& projection, const Matrix& view,
    float nearPlane, const float farPlane, const UINT width, const UINT height, const std::vector<GDX12LocalLightConstants>& lights,
    JobSystem* jobs, GDX12LightGridSettings settings)
{
    EnsureIdle();
    GDX12_SCOPED_CPU_EVENT("CPU light grid");
    if (!width || !height || width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION || height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        !std::isfinite(nearPlane) || nearPlane <= 0 ||
        !(farPlane == 0 || farPlane > nearPlane) || XMMatrixIsNaN(projection) || XMMatrixIsInfinite(projection) ||
        XMMatrixIsNaN(view) || XMMatrixIsInfinite(view) || projection._11 <= 0 || projection._22 <= 0 ||
        projection._34 != -1 || projection._44 != 0 || lights.size() > UINT_MAX ||
        settings.TileSize < 8 || settings.TileSize > 1024 || settings.DepthSlices < 1 || settings.DepthSlices > 128 ||
        !std::isfinite(settings.FarDepthCm) || settings.FarDepthCm < 1 || settings.FarDepthCm > 1e9f)
        throw std::invalid_argument("Invalid perspective light grid inputs");
    const bool infinite = farPlane == 0 || std::isinf(farPlane);
    const float gridFar = (std::max)(nearPlane * 2, infinite ? settings.FarDepthCm : (std::min)(farPlane, settings.FarDepthCm));
    if (!std::isfinite(gridFar)) throw std::invalid_argument("Invalid light grid depth range");
    grid.Constants.Size = { (width - 1) / settings.TileSize + 1, (height - 1) / settings.TileSize + 1,
        settings.DepthSlices, static_cast<UINT>((lights.size() + 31) / 32) };
    grid.Constants.Depth = { settings.DepthSlices / std::log2(gridFar / nearPlane), 0, float(settings.TileSize), nearPlane };
    grid.Constants.Depth.y = -std::log2(nearPlane) * grid.Constants.Depth.x;
    const size_t cells = size_t(grid.Constants.Size.x) * grid.Constants.Size.y * settings.DepthSlices;
    if (grid.Constants.Size.w && cells > (UINT_MAX / sizeof(UINT)) / grid.Constants.Size.w)
        throw std::length_error("Light grid exceeds the upload buffer size limit");
    grid.Masks.resize((std::max)(size_t(1), cells * grid.Constants.Size.w));
    _grid = &grid; _projection = projection; _width = width; _height = height;
    _z.resize(size_t(settings.DepthSlices) + 1);
    for (UINT slice = 0; slice <= settings.DepthSlices; ++slice)
        _z[slice] = nearPlane * std::exp2(float(slice) / grid.Constants.Depth.x);
    _z[0] = nearPlane;
    _z[settings.DepthSlices] = infinite ? (std::numeric_limits<float>::max)() : farPlane;
    _spheres.clear();
    _spheres.reserve(lights.size());
    for (UINT id = 0; id < lights.size(); ++id)
    {
        const auto& light = lights[id];
        const Vector3 p = Vector3::Transform(Vector3(light.Position), view);
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) ||
            !std::isfinite(light.Range) || light.Range <= 0)
            throw std::invalid_argument("Invalid light grid sphere");
        const float z = -p.z, r = light.Range;
        const float margin = (std::max)(std::abs(z), r) * 1e-5f;
        const float lo = (std::max)(nearPlane, z - r - margin);
        const float hi = (std::min)(_z[settings.DepthSlices], z + r + margin);
        if (hi >= lo) _spheres.push_back({ p, r, lo, hi, margin, grid.DepthSlice(lo), grid.DepthSlice(hi), id });
    }
    if (!grid.Constants.Size.w) { grid.Masks[0] = 0; return; }
    if (jobs)
    {
        // Cap the batch size so a small number of slices can still occupy the shared pool.
        const size_t batch = (std::min)(jobs->GetDefaultBatchSize(),
            (std::max)(size_t(1), size_t(settings.DepthSlices) / jobs->GetThreadCount()));
        ParallelJob::Schedule(*jobs, settings.DepthSlices, batch).Complete();
    }
    else for (UINT slice = 0; slice < settings.DepthSlices; ++slice) Execute(slice);
}

void GDX12LightGridBuildSystem::Execute(const size_t slice)
{
    const auto size = _grid->Constants.Size;
    const float tileSize = _grid->Constants.Depth.z;
    const size_t sliceWords = size_t(size.x) * size.y * size.w;
    std::fill_n(_grid->Masks.begin() + slice * sliceWords, sliceWords, 0u);
    for (const auto& sphere : _spheres)
    {
        if (slice < sphere.First || slice > sphere.Last) continue;
        const float front = (std::max)(sphere.Lo, _z[slice]), back = (std::min)(sphere.Hi, _z[slice + 1]);
        if (back < front) continue;
        const float z = -sphere.Center.z;
        // Clip the sphere to this Z slab before projecting conservative XY bounds.
        const float dz = std::abs((std::clamp)(z, front, back) - z) / sphere.Radius;
        const float radial = sphere.Radius * std::sqrt((std::max)(0.0f, 1 - dz * dz)) + sphere.Margin;
        const auto interval = [&](const float center, const float scale, const float offset)
        {
            const float a = (center - radial) / front, b = (center - radial) / back;
            const float c = (center + radial) / front, d = (center + radial) / back;
            return Vector2((std::min)(a, b) * scale - offset, (std::max)(c, d) * scale - offset);
        };
        const auto x = interval(sphere.Center.x, _projection._11, _projection._31);
        const auto y = interval(sphere.Center.y, _projection._22, _projection._32);
        if (x.y < -1 || x.x > 1 || y.y < -1 || y.x > 1) continue;
        const auto cell = [&](const float pixel, const UINT count)
        {
            return static_cast<UINT>((std::clamp)(std::floor(pixel / tileSize), 0.0f, float(count - 1)));
        };
        const UINT x0 = cell((x.x + 1) * 0.5f * _width - 1e-3f, size.x);
        const UINT x1 = cell((x.y + 1) * 0.5f * _width + 1e-3f, size.x);
        const UINT y0 = cell((1 - y.y) * 0.5f * _height - 1e-3f, size.y);
        const UINT y1 = cell((1 - y.x) * 0.5f * _height + 1e-3f, size.y);
        for (UINT row = y0; row <= y1; ++row)
        {
            size_t address = ((slice * size.y + row) * size.x + x0) * size.w + sphere.Id / 32;
            for (UINT column = x0; column <= x1; ++column, address += size.w)
                _grid->Masks[address] |= 1u << (sphere.Id & 31);
        }
    }
}

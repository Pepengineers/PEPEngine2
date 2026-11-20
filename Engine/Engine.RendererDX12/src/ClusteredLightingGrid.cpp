#include "Engine.RendererDX12/ClusteredLightingGrid.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <stdexcept>

void ClusteredLightingGrid::BuildClusterLightMasks(const Matrix& view, const Matrix& projection,
                                    float nearPlane, float farPlane, unsigned width, unsigned height)
{
    constexpr unsigned LightTileSize = 64;
    constexpr unsigned LightDepthSlices = 32;
    constexpr unsigned LightsPerMaskWord = 32;
    const unsigned columns = (width + LightTileSize - 1) / LightTileSize;
    const unsigned rows = (height + LightTileSize - 1) / LightTileSize;
    const auto wordsPerCluster = static_cast<unsigned>((Lights.size() + LightsPerMaskWord - 1) / LightsPerMaskWord);
    const float depthScale = LightDepthSlices / std::log2(farPlane / nearPlane);
    Constants = {columns, rows, wordsPerCluster, LightTileSize, LightDepthSlices,
                  depthScale, -std::log2(nearPlane) * depthScale};

    const std::uint64_t wordCount = std::uint64_t(columns) * rows * LightDepthSlices * wordsPerCluster;
    if (wordCount > UINT_MAX / sizeof(std::uint32_t)) { throw std::length_error("Light grid exceeds upload buffer capacity"); }
    ClusterLightMasks.assign(static_cast<size_t>(wordCount), 0);
    if (wordCount == 0) { return; }

    float sliceDepths[LightDepthSlices + 1];
    for (unsigned i = 0; i < LightDepthSlices; ++i)
    {
        sliceDepths[i] = nearPlane * std::exp2(i / depthScale);
    }
    sliceDepths[LightDepthSlices] = farPlane;

    auto pixelToTile = [](float pixel, unsigned tileCount)
    {
        return static_cast<unsigned>(std::clamp(std::floor(pixel / LightTileSize), 0.0f, float(tileCount - 1)));
    };

    for (const UINT lightIndex : ActiveLightIndices)
    {
        const auto& light = Lights[lightIndex];
        if (light.Intensity <= 0) { continue; }
        const unsigned maskWord = lightIndex / LightsPerMaskWord;
        const unsigned lightBit = 1u << (lightIndex % LightsPerMaskWord);
        if (light.Type == LightType::Directional)
        {
            for (size_t word = maskWord; word < ClusterLightMasks.size(); word += wordsPerCluster)
                ClusterLightMasks[word] |= lightBit;
            continue;
        }
        if (light.Range <= 0) { continue; }

        // Bistro's conservative sphere bounds, clipped to each logarithmic depth slice.
        const Vector3 center = Vector3::Transform(light.Position, view);
        const float centerDepth = -center.z;
        const float range = light.Range;
        const float margin = (std::max)(std::abs(centerDepth), range) * 1e-5f;
        for (unsigned slice = 0; slice < LightDepthSlices; ++slice)
        {
            const float front = (std::max)(sliceDepths[slice], centerDepth - range - margin);
            const float back = (std::min)(sliceDepths[slice + 1], centerDepth + range + margin);
            if (back < front) { continue; }
            const float depthOffset = (std::clamp(centerDepth, front, back) - centerDepth) / range;
            const float sliceRadius = range * std::sqrt((std::max)(0.0f, 1 - depthOffset * depthOffset)) + margin;
            auto projectInterval = [&](float coordinate, float focal, float offset)
            {
                const float low = coordinate - sliceRadius;
                const float high = coordinate + sliceRadius;
                return Vector2((std::min)(low / front, low / back) * focal - offset,
                               (std::max)(high / front, high / back) * focal - offset);
            };
            const auto x = projectInterval(center.x, projection._11, projection._31);
            const auto y = projectInterval(center.y, projection._22, projection._32);
            if (x.y < -1 || x.x > 1 || y.y < -1 || y.x > 1) { continue; }
            const unsigned left = pixelToTile((1 + x.x) * 0.5f * width - 0.001f, columns);
            const unsigned right = pixelToTile((1 + x.y) * 0.5f * width + 0.001f, columns);
            const unsigned top = pixelToTile((1 - y.y) * 0.5f * height - 0.001f, rows);
            const unsigned bottom = pixelToTile((1 - y.x) * 0.5f * height + 0.001f, rows);
            for (unsigned row = top; row <= bottom; ++row)
            {
                for (unsigned col = left; col <= right; ++col)
                {
                    const unsigned clusterIndex = (slice * rows + row) * columns + col;
                    ClusterLightMasks[clusterIndex * wordsPerCluster + maskWord] |= lightBit;
                }
            }
        }
    }
}

#pragma once
#include "Engine.RendererDX12/GDX12ConstantStructures.h"

// A set bit in a cluster indexes the same light in Lights and the GPU LightsCache.
struct ClusteredLightingGrid
{
    std::vector<GDX12LightConstants> Lights;
    std::vector<std::uint32_t> ClusterLightMasks;
    GDX12LightGridConstants Constants = {};

    void BuildClusterLightMasks(const Matrix& view, const Matrix& projection,
                                float nearPlane, float farPlane, unsigned width, unsigned height);
};

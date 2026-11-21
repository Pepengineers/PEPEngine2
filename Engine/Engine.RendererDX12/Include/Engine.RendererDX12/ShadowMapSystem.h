#pragma once

#include "Engine.RendererDX12/GDX12ConstantStructures.h"
#include <unordered_map>
#include <vector>

// Prepares shadow projections on the CPU: four cascades per directional light,
// six cube faces per point light and one view per spot light.
// Keeps world-space caster bounds from all worlds and the active camera parameters.
// GPUDataUpdateSystem refreshes point/spot projections when their light or view indices change.
// PreRender rebuilds directional cascades after all worlds have updated, assigns texture-array
// layers and uploads Views to each GPU that uses lighting. FirstShadowViewIndex in LightsCache
// points to the light's first entry in Views. Shadow-map textures and rendering belong to passes.
class ShadowMapSystem
{
public:
    static constexpr UINT CascadeResolution = 2048;
    static constexpr UINT PointSpotResolution = 256;

    // Records the active camera for projection building after all worlds have updated their casters.
    void SetCamera(const Matrix& view, const Matrix& projection, float nearPlane, float farPlane);

    // Keeps world-space submesh bounds for directional fitting; instance slots distinguish casters across worlds.
    void UpdateCaster(UINT instance, const BoundingBox& bounds);

    // Instance indices are global draw-cache slots, so casters from different worlds stay distinct.
    void RemoveCaster(UINT instance);

    // Updates cached projections when a point/spot light or its FirstShadowViewIndex changes; follows Bistro GDX12LocalLights.
    void UpdatePointSpotViews(const GDX12LightConstants& light);

    // Merges caster bounds after edits, rebuilds directional cascades each frame and assigns array layers,
    // preserving cached point/spot projections.
    void BuildViews(const std::vector<GDX12LightConstants>& lights);

    std::vector<ShadowView> Views;
    UINT DirectionalArraySize = 0;
    UINT PointSpotArraySize = 0;

private:
    // Cascade splits, texel snapping and overlap follow Bistro GDX12ShadowCascades.
    void UpdateDirectionalViews(const GDX12LightConstants& light, const BoundingBox* casterBounds);

    // Draw cache instance to bounds map
    std::unordered_map<UINT, BoundingBox> _casters;
    BoundingBox _casterBounds{};
    bool _casterBoundsDirty = false;
    Matrix _inverseView = Matrix::Identity;
    Matrix _projection = Matrix::Identity;
    float _nearPlane = .1f;
    float _farPlane = 10000.f;
};

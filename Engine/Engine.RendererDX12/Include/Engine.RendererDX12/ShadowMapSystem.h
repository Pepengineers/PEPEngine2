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
// Collects changed caster bounds and projection edits to invalidate point/spot static and final maps.
// Each affected point light updates its six faces together; stationary dynamic casters stay cached.
class ShadowMapSystem
{
public:
    static constexpr UINT CascadeResolution = 2048;
    static constexpr UINT PointSpotResolution = 256;

    // Records the active camera for projection building after all worlds have updated their casters.
    void SetCamera(const Matrix& view, const Matrix& projection, float nearPlane, float farPlane);

    // Keeps world-space bounds for fitting and cache invalidation. Both the old and new bounds
    // invalidate affected lights; the first movement also removes the caster from the static cache.
    void UpdateCaster(UINT instance, const BoundingBox& bounds, bool dynamic = false);

    // Instance indices are global draw-cache slots, so casters from different worlds stay distinct.
    void RemoveCaster(UINT instance);

    // Updates cached projections when a point/spot light or its FirstShadowViewIndex changes; follows Bistro GDX12LocalLights.
    void UpdatePointSpotViews(const GDX12LightConstants& light);

    // Merges caster bounds, rebuilds directional cascades and assigns array layers.
    // Final-cache changes stay dirty for frameSlotCount consecutive rendered frames, so every
    // cyclic FrameGraph slot receives the latest depth. Static-cache changes apply once.
    void BuildViews(const std::vector<GDX12LightConstants>& lights, UINT frameSlotCount);

    std::vector<ShadowView> Views;
    // One cache-update mask per light, indexed like LightsCache. All views of a light share this entry.
    std::vector<UINT> LightUpdateFlags;
    UINT DirectionalArraySize = 0;
    UINT PointSpotArraySize = 0;

private:
    // Cascade splits, texel snapping and overlap follow Bistro GDX12ShadowCascades.
    void UpdateDirectionalViews(const GDX12LightConstants& light, const BoundingBox* casterBounds);

    struct ShadowCaster
    {
        BoundingBox Bounds;
        bool Dynamic;
    };
    struct ChangedCasterBounds
    {
        BoundingBox Bounds;
        bool Static;
    };
    std::unordered_map<UINT, ShadowCaster> _casters;
    std::vector<ChangedCasterBounds> _changedCasterBounds;
    // Pending projection changes, keyed by the light's first view; consumed by BuildViews.
    std::unordered_map<UINT, UINT> _pointSpotUpdates;
    // One counter per light, following the same propagation pattern as component _numFramesDirty.
    std::vector<UINT> _finalFramesDirty;
    BoundingBox _casterBounds{};
    bool _casterBoundsDirty = false;
    Matrix _inverseView = Matrix::Identity;
    Matrix _projection = Matrix::Identity;
    float _nearPlane = .1f;
    float _farPlane = 10000.f;
};

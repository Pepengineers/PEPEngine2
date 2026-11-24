#include "Engine.RendererDX12/ShadowMapSystem.h"
#include <algorithm>
#include <cmath>

void ShadowMapSystem::SetCamera(const Matrix& view, const Matrix& projection, float nearPlane, float farPlane)
{
    _inverseView = view.Invert();
    _projection = projection;
    _nearPlane = nearPlane;
    _farPlane = farPlane;
}

void ShadowMapSystem::UpdateCaster(UINT instance, const BoundingBox& bounds, bool dynamic)
{
    const auto previous = _casters.find(instance);
    if (previous != _casters.end())
        _changedCasterBounds.push_back({previous->second.Bounds, !previous->second.Dynamic});
    _changedCasterBounds.push_back({bounds, !dynamic});
    _casters[instance] = {bounds, dynamic};
    _casterBoundsDirty = true;
}

void ShadowMapSystem::RemoveCaster(UINT instance)
{
    const auto caster = _casters.find(instance);
    if (caster == _casters.end()) return;
    _changedCasterBounds.push_back({caster->second.Bounds, !caster->second.Dynamic});
    _casters.erase(caster);
    _casterBoundsDirty = true;
}

void ShadowMapSystem::BuildViews(const std::vector<GDX12LightConstants>& lights, UINT frameSlotCount)
{
    UINT viewCount = 0;
    UINT pointSpotCount = 0;
    for (const auto& light : lights)
    {
        if (light.FirstShadowViewIndex != UINT_MAX)
        {
            viewCount += light.Type == LightType::Directional ? 4 : light.Type == LightType::Point ? 6 : 1;
            if (light.Type != LightType::Directional) pointSpotCount += light.Type == LightType::Point ? 6 : 1;
        }
    }
    Views.resize(viewCount);
    LightUpdateFlags.assign(lights.size(), 0);
    if (_finalFramesDirty.size() != lights.size()) _finalFramesDirty.assign(lights.size(), frameSlotCount);
    const bool arrayChanged = pointSpotCount != PointSpotArraySize;
    DirectionalArraySize = PointSpotArraySize = 0;
    if (_casterBoundsDirty)
    {
        _casterBounds = {};
        if (!_casters.empty())
        {
            _casterBounds = _casters.begin()->second.Bounds;
            for (const auto& [instance, caster] : _casters)
                BoundingBox::CreateMerged(_casterBounds, _casterBounds, caster.Bounds);
        }
        _casterBoundsDirty = false;
    }
    for (UINT lightIndex = 0; lightIndex < lights.size(); ++lightIndex)
    {
        const auto& light = lights[lightIndex];
        if (light.FirstShadowViewIndex == UINT_MAX) continue;
        if (light.Type == LightType::Directional)
            UpdateDirectionalViews(light, _casters.empty() ? nullptr : &_casterBounds);
        else
        {
            UINT updates = arrayChanged ? SHADOW_UPDATE_STATIC | SHADOW_UPDATE_FINAL : 0;
            const auto projection = _pointSpotUpdates.find(light.FirstShadowViewIndex);
            if (projection != _pointSpotUpdates.end()) updates |= projection->second;
            const BoundingSphere influence(light.Position, light.Range);
            for (const auto& change : _changedCasterBounds)
            {
                if (!influence.Intersects(change.Bounds)) continue;
                updates |= SHADOW_UPDATE_FINAL;
                if (change.Static) updates |= SHADOW_UPDATE_STATIC;
            }
            if (updates & SHADOW_UPDATE_FINAL) _finalFramesDirty[lightIndex] = frameSlotCount;
            if (_finalFramesDirty[lightIndex])
            {
                updates |= SHADOW_UPDATE_FINAL;
                --_finalFramesDirty[lightIndex];
            }
            LightUpdateFlags[lightIndex] = updates;
        }
        const UINT count = light.Type == LightType::Directional ? 4 : light.Type == LightType::Point ? 6 : 1;
        for (UINT face = 0; face < count; ++face)
        {
            auto& view = Views[light.FirstShadowViewIndex + face];
            view.LightIndex = lightIndex;
            if (light.Type != LightType::Directional) view.Layer = PointSpotArraySize++;
        }
    }
    _pointSpotUpdates.clear();
    _changedCasterBounds.clear();
}

void ShadowMapSystem::UpdateDirectionalViews(const GDX12LightConstants& light, const BoundingBox* casterBounds)
{
    const float shadowFar = (std::min)(_farPlane, (std::max)(6000.f, _nearPlane * 2.f));
    const Vector3 up = std::abs(light.Direction.y) > .99f ? Vector3::UnitZ : Vector3::UnitY;
    const Matrix lightView = Matrix::CreateLookAt(Vector3::Zero, light.Direction, up);
    float previous = _nearPlane;
    for (UINT cascade = 0; cascade < 4; ++cascade)
    {
        const float fraction = (cascade + 1) / 4.f;
        const float split = .7f * _nearPlane * std::pow(shadowFar / _nearPlane, fraction) +
                            .3f * (_nearPlane + (shadowFar - _nearPlane) * fraction);
        Vector3 corners[8], center{};
        UINT index = 0;
        for (float depth : {previous, split})
            for (float y : {-1.f, 1.f})
                for (float x : {-1.f, 1.f})
                {
                    corners[index] = Vector3::Transform({x * depth / _projection._11, y * depth / _projection._22, -depth}, _inverseView);
                    center += corners[index++];
                }
        center /= 8.f;
        float radius = 0;
        for (const auto& corner : corners) radius = (std::max)(radius, Vector3::Distance(center, corner));
        radius = std::ceil(radius / 16.f) * 16.f * DirectionalLightShadowMapResolution / (DirectionalLightShadowMapResolution - 2.f);
        const float texelSize = 2.f * radius / DirectionalLightShadowMapResolution;
        center = Vector3::Transform(center, lightView);
        center.x = std::floor(center.x / texelSize) * texelSize;
        center.y = std::floor(center.y / texelSize) * texelSize;
        const float depthRadius = radius + 4000.f;
        float nearDepth = -center.z - depthRadius;
        float farDepth = -center.z + depthRadius;
        if (casterBounds)
        {
            BoundingBox lightBounds;
            casterBounds->Transform(lightBounds, lightView);
            const Vector3 low = Vector3(lightBounds.Center) - Vector3(lightBounds.Extents);
            const Vector3 high = Vector3(lightBounds.Center) + Vector3(lightBounds.Extents);
            if (high.x >= center.x - radius && low.x <= center.x + radius &&
                high.y >= center.y - radius && low.y <= center.y + radius)
            {
                nearDepth = (std::min)(nearDepth, -high.z - 1.f);
                farDepth = (std::max)(farDepth, -low.z + 1.f);
            }
        }
        const Matrix projection = Matrix::CreateOrthographicOffCenter(center.x - radius, center.x + radius,
            center.y - radius, center.y + radius, nearDepth, farDepth);
        ShadowView view{};
        XMStoreFloat4x4(&view.ViewProj, (lightView * projection).Transpose());
        view.Layer = DirectionalArraySize++;
        view.Type = light.Type;
        view.SplitDepth = split;
        view.NormalBias = texelSize;
        Views[light.FirstShadowViewIndex + cascade] = view;
        previous = split - .1f * (split - previous);
    }
}

void ShadowMapSystem::UpdatePointSpotViews(const GDX12LightConstants& light)
{
    _pointSpotUpdates[light.FirstShadowViewIndex] = SHADOW_UPDATE_STATIC | SHADOW_UPDATE_FINAL;
    const bool point = light.Type == LightType::Point;
    const UINT count = point ? 6 : 1;
    const UINT end = light.FirstShadowViewIndex + count;
    if (Views.size() < end) Views.resize(end);
    const Vector3 directions[] = {Vector3::UnitX, -Vector3::UnitX, Vector3::UnitY, -Vector3::UnitY, Vector3::UnitZ, -Vector3::UnitZ};
    const Vector3 ups[] = {Vector3::UnitY, Vector3::UnitY, -Vector3::UnitZ, Vector3::UnitZ, Vector3::UnitY, Vector3::UnitY};
    const float fov = point ? XM_PIDIV2 : 2.f * std::acos(light.OuterConeCos);
    const Matrix projection = Matrix::CreatePerspectiveFieldOfView(fov, 1.f, (std::min)(.5f, light.Range * .5f), light.Range);
    for (UINT face = 0; face < count; ++face)
    {
        const Vector3 direction = point ? directions[face] : light.Direction;
        const Vector3 up = point ? ups[face] : std::abs(direction.y) > .99f ? Vector3::UnitZ : Vector3::UnitY;
        const Matrix viewMatrix = Matrix::CreateLookAt(light.Position, light.Position + direction, up);
        ShadowView view{};
        XMStoreFloat4x4(&view.ViewProj, (viewMatrix * projection).Transpose());
        view.Type = light.Type;
        view.NormalBias = light.Range / PointSpotLightShadowMapResolution;
        Views[light.FirstShadowViewIndex + face] = view;
    }
}

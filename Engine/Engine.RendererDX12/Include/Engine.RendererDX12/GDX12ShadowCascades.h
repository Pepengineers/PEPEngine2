#pragma once

#include "Engine.RendererDX12/GDX12LocalLights.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

inline GDX12ShadowConstants BuildShadowCascades(const Matrix& inverseView, float fov, float aspect,
    float nearPlane, float farPlane, Vector3 lightDirection, const BoundingBox* casterBounds = nullptr,
    UINT resolution = SunShadowCascadeResolution, const GDX12SunCascadeSettings& settings = {})
{
    GDX12ShadowConstants result;
    result.CascadeCount = 0;
    if (settings.Count < 1 || settings.Count > 4 || !std::isfinite(settings.MaxDistance) || settings.MaxDistance < 0)
        throw std::invalid_argument("Invalid sun cascade settings");
    const float shadowFar = farPlane > 0 ? (std::min)(farPlane, settings.MaxDistance) : settings.MaxDistance;
    if (resolution == 0 || shadowFar <= nearPlane) return result;
    if (resolution <= 2 || !std::isfinite(nearPlane) || nearPlane <= 0)
        throw std::invalid_argument("Invalid sun cascade resolution or near plane");
    float splits[4] = {shadowFar, shadowFar, shadowFar, shadowFar}, texelSizes[4] = {};
    const float ratios[] = {settings.Splits.x, settings.Splits.y, settings.Splits.z};
    if (!settings.AutomaticSplits)
        for (UINT i = 0; i + 1 < settings.Count; ++i)
            if (!std::isfinite(ratios[i]) || ratios[i] <= (i ? ratios[i - 1] : 0) || ratios[i] >= 1)
                throw std::invalid_argument("Sun cascade splits must be ordered fractions between zero and one");
    result.CascadeCount = settings.Count;
    const Vector3 up = std::abs(lightDirection.y) > 0.99f ? Vector3::UnitZ : Vector3::UnitY;
    lightDirection.Normalize();
    const Matrix lightView = Matrix::CreateLookAt(Vector3::Zero, lightDirection, up);
    float previous = nearPlane;
    for (UINT cascade = 0; cascade < settings.Count; ++cascade)
    {
        const float fraction = float(cascade + 1) / settings.Count;
        const float split = cascade + 1 == settings.Count ? shadowFar : settings.AutomaticSplits ?
            0.7f * nearPlane * std::pow(shadowFar / nearPlane, fraction) + 0.3f * (nearPlane + (shadowFar - nearPlane) * fraction) :
            nearPlane + (shadowFar - nearPlane) * ratios[cascade];
        splits[cascade] = split;
        Vector3 corners[8];
        int index = 0;
        for (float depth : { previous, split })
        {
            const float height = depth * std::tan(fov * 0.5f);
            for (float y : { -height, height })
                for (float x : { -height * aspect, height * aspect })
                    corners[index++] = Vector3::Transform(Vector3(x, y, -depth), inverseView);
        }
        Vector3 center = Vector3::Zero;
        for (const auto& corner : corners) center += corner;
        center /= 8.0f;
        float radius = 0.0f;
        for (const auto& corner : corners) radius = (std::max)(radius, Vector3::Distance(center, corner));
        radius = std::ceil(radius / 16.0f) * 16.0f;
        // Reserve a texel around the sphere for snapping and PCF at cascade edges.
        radius *= static_cast<float>(resolution) / static_cast<float>(resolution - 2);
        const float texelSize = 2.0f * radius / resolution;
        texelSizes[cascade] = texelSize;
        Vector3 centerLS = Vector3::Transform(center, lightView);
        centerLS.x = std::floor(centerLS.x / texelSize) * texelSize;
        centerLS.y = std::floor(centerLS.y / texelSize) * texelSize;
        // Include off-camera casters above/below the receiver frustum.
        const float depthRadius = radius + 4000.0f;
        float shadowNear = -centerLS.z - depthRadius;
        float shadowFarPlane = -centerLS.z + depthRadius;
        if (casterBounds)
        {
            BoundingBox lightBounds;
            casterBounds->Transform(lightBounds, lightView);
            const Vector3 low = Vector3(lightBounds.Center) - Vector3(lightBounds.Extents);
            const Vector3 high = Vector3(lightBounds.Center) + Vector3(lightBounds.Extents);
            if (high.x >= centerLS.x - radius && low.x <= centerLS.x + radius &&
                high.y >= centerLS.y - radius && low.y <= centerLS.y + radius)
            {
                shadowNear = (std::min)(shadowNear, -high.z - 1);
                shadowFarPlane = (std::max)(shadowFarPlane, -low.z + 1);
            }
        }
        const Matrix projection = Matrix::CreateOrthographicOffCenter(
            centerLS.x - radius, centerLS.x + radius, centerLS.y - radius, centerLS.y + radius,
            shadowNear, shadowFarPlane);
        XMStoreFloat4x4(&result.ViewProj[cascade], XMMatrixTranspose(lightView * projection));
        // Overlap receiver intervals for a smooth transition between cascades.
        previous = split - (split - previous) * 0.1f;
    }
    result.SplitDepths = XMFLOAT4(splits[0], splits[1], splits[2], splits[3]);
    result.TexelWorldSize = XMFLOAT4(texelSizes[0], texelSizes[1], texelSizes[2], texelSizes[3]);
    return result;
}

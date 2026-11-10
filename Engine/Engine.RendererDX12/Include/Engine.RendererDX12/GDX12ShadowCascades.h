#pragma once

#include "Engine.RendererDX12/GDX12LocalLights.h"
#include <algorithm>
#include <cmath>

inline GDX12ShadowConstants BuildShadowCascades(const Matrix& inverseView, float fov, float aspect,
    float nearPlane, float farPlane, Vector3 lightDirection, const BoundingBox* casterBounds = nullptr,
    UINT resolution = SunShadowCascadeResolution)
{
    GDX12ShadowConstants result;
    if (resolution == 0) return result;
    float splits[4], texelSizes[4];
    const float shadowFar = (std::min)(farPlane, 6000.0f);
    const Vector3 up = std::abs(lightDirection.y) > 0.99f ? Vector3::UnitZ : Vector3::UnitY;
    lightDirection.Normalize();
    const Matrix lightView = Matrix::CreateLookAt(Vector3::Zero, lightDirection, up);
    float previous = nearPlane;
    for (int cascade = 0; cascade < 4; ++cascade)
    {
        const float fraction = (cascade + 1) / 4.0f;
        const float split = 0.7f * nearPlane * std::pow(shadowFar / nearPlane, fraction) +
            0.3f * (nearPlane + (shadowFar - nearPlane) * fraction);
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

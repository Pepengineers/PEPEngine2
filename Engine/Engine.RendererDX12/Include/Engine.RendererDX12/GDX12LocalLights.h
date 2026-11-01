#pragma once
#include "Engine.RendererDX12/GDX12ConstantStructures.h"
#include <cstring>
#include <cmath>
#include <vector>

inline constexpr UINT LocalShadowResolution = 256;

struct GDX12LocalShadowCaster
{
    BoundingBox Bounds;
    uint64_t Signature = 0;
    bool Dynamic = false;
};

inline bool LocalShadowIntersects(const GDX12LocalLightConstants& light, const BoundingBox& bounds)
{
    if (!BoundingSphere(light.Position, light.Range).Intersects(bounds)) return false;
    if (light.Type == 1) return true;
    // Uploaded matrices are transposed: their rows are the clip-space plane coefficients.
    const XMMATRIX m = XMLoadFloat4x4(&light.ShadowViewProj[0]);
    const XMVECTOR planes[] = { m.r[3] + m.r[0], m.r[3] - m.r[0], m.r[3] + m.r[1],
        m.r[3] - m.r[1], m.r[2], m.r[3] - m.r[2] };
    const auto center = XMLoadFloat3(&bounds.Center);
    const auto extents = XMLoadFloat3(&bounds.Extents);
    for (const auto plane : planes)
        if (XMVectorGetX(XMVector3Dot(center, plane)) + XMVectorGetW(plane) +
            XMVectorGetX(XMVector3Dot(extents, XMVectorAbs(plane))) < 0) return false;
    return true;
}

inline uint64_t LocalShadowCasterSignature(const GDX12LocalLightConstants& light,
    const std::vector<GDX12LocalShadowCaster>& casters, bool staticOnly)
{
    uint64_t signature = 14695981039346656037ull;
    const auto hash = [&](const auto& value)
    {
        const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
        for (size_t i = 0; i < sizeof(value); ++i) signature = (signature ^ bytes[i]) * 1099511628211ull;
    };
    for (const auto& caster : casters)
    {
        if ((staticOnly && caster.Dynamic) || !LocalShadowIntersects(light, caster.Bounds)) continue;
        hash(caster.Signature);
        hash(caster.Bounds.Center);
        hash(caster.Bounds.Extents);
        hash(caster.Dynamic);
    }
    // Comparing the old and new caster sets also catches exits, removals and static/dynamic reclassification.
    return signature;
}

inline UINT LocalShadowFaceCount(const GDX12LocalLightConstants& light)
{
    return light.Type == 1 ? 6u : 1u;
}

inline void BuildLocalLightShadowMatrices(GDX12LocalLightConstants& light)
{
    const Vector3 position(light.Position);
    const Vector3 directions[] = { Vector3::UnitX, -Vector3::UnitX, Vector3::UnitY, -Vector3::UnitY, Vector3::UnitZ, -Vector3::UnitZ };
    const Vector3 ups[] = { Vector3::UnitY, Vector3::UnitY, -Vector3::UnitZ, Vector3::UnitZ, Vector3::UnitY, Vector3::UnitY };
    const float fov = light.Type == 1 ? XM_PIDIV2 : 2.0f * std::acos(light.OuterCos);
    const Matrix projection = Matrix::CreatePerspectiveFieldOfView(fov, 1.0f, 0.5f, light.Range);
    for (UINT face = 0; face < LocalShadowFaceCount(light); ++face)
    {
        Vector3 direction = light.Type == 1 ? directions[face] : Vector3(light.Direction);
        Vector3 up = light.Type == 1 ? ups[face] : (std::abs(direction.y) > 0.99f ? Vector3::UnitZ : Vector3::UnitY);
        const Matrix view = Matrix::CreateLookAt(position, position + direction, up);
        XMStoreFloat4x4(&light.ShadowViewProj[face], XMMatrixTranspose(view * projection));
    }
}

inline bool ShouldRenderLocalShadow(const GDX12LocalLightConstants& light, const GDX12LocalLightConstants* cached)
{
    if (light.ShadowFirstSlice == UINT_MAX) return false;
    if ((light.ShadowFlags & 2u) || !cached) return true;
    return light.EntityId != cached->EntityId || light.ShadowRevision != cached->ShadowRevision ||
        light.Type != cached->Type || light.ShadowFirstSlice != cached->ShadowFirstSlice ||
        std::memcmp(light.ShadowViewProj, cached->ShadowViewProj, sizeof(light.ShadowViewProj)) != 0;
}

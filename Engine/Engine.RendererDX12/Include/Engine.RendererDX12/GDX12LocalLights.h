#pragma once
#include "Engine.RendererDX12/GDX12ConstantStructures.h"
#include <cstring>
#include <cmath>
#include <vector>
#include <array>

inline constexpr UINT LocalShadowResolution = 256;

struct GDX12LocalShadowCaster
{
    BoundingBox Bounds;
    uint64_t Signature = 0;
    bool Dynamic = false;
};

struct GDX12LocalShadowVisibility
{
    // One signature per face; each render pass compares against the last contents of its own maps.
    std::array<uint64_t, 6> StaticCasters = {};
    std::array<uint64_t, 6> AllCasters = {};
};

inline UINT LocalShadowFaceCount(const GDX12LocalLightConstants& light)
{
    return light.Type == 1 ? 6u : 1u;
}

inline bool LocalShadowFaceIntersects(const XMFLOAT4X4& viewProjection, const BoundingBox& bounds)
{
    // Uploaded matrices are transposed: their rows are the clip-space plane coefficients.
    const XMMATRIX m = XMLoadFloat4x4(&viewProjection);
    const XMVECTOR planes[] = { m.r[3] + m.r[0], m.r[3] - m.r[0], m.r[3] + m.r[1],
        m.r[3] - m.r[1], m.r[2], m.r[3] - m.r[2] };
    const auto center = XMLoadFloat3(&bounds.Center);
    const auto extents = XMLoadFloat3(&bounds.Extents);
    for (const auto plane : planes)
        if (XMVectorGetX(XMVector3Dot(center, plane)) + XMVectorGetW(plane) +
            XMVectorGetX(XMVector3Dot(extents, XMVectorAbs(plane))) < 0) return false;
    return true;
}

inline bool LocalShadowIntersects(const GDX12LocalLightConstants& light, const BoundingBox& bounds)
{
    return BoundingSphere(light.Position, light.Range).Intersects(bounds) &&
        (light.Type == 1 || LocalShadowFaceIntersects(light.ShadowViewProj[0], bounds));
}

inline GDX12LocalShadowVisibility BuildLocalShadowVisibility(const GDX12LocalLightConstants& light,
    const std::vector<GDX12LocalShadowCaster>& casters)
{
    GDX12LocalShadowVisibility result;
    result.StaticCasters.fill(14695981039346656037ull);
    result.AllCasters = result.StaticCasters;
    if (light.ShadowFirstSlice == UINT_MAX) return result;
    const auto hash = [](uint64_t& signature, const auto& value)
    {
        const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
        for (size_t i = 0; i < sizeof(value); ++i) signature = (signature ^ bytes[i]) * 1099511628211ull;
    };
    for (const auto& caster : casters)
    {
        if (!BoundingSphere(light.Position, light.Range).Intersects(caster.Bounds)) continue;
        uint64_t signature = caster.Signature;
        hash(signature, caster.Bounds.Center);
        hash(signature, caster.Bounds.Extents);
        hash(signature, caster.Dynamic);
        for (UINT face = 0; face < LocalShadowFaceCount(light); ++face)
        {
            if (!LocalShadowFaceIntersects(light.ShadowViewProj[face], caster.Bounds)) continue;
            hash(result.AllCasters[face], signature);
            if (!caster.Dynamic) hash(result.StaticCasters[face], signature);
        }
    }
    // Comparing the old and new caster sets also catches exits, removals and static/dynamic reclassification.
    return result;
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

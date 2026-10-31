#pragma once
#include "Engine.RendererDX12/GDX12ConstantStructures.h"
#include <cstring>
#include <cmath>

inline constexpr UINT LocalShadowResolution = 256;

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

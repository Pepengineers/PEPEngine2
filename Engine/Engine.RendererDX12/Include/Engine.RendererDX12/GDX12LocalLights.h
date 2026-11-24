#pragma once
#include "Engine.RendererDX12/GDX12ConstantStructures.h"
#include <cstring>
#include <cmath>
#include <vector>
#include <array>
#include <optional>
#include <string>
#include <string_view>

inline constexpr UINT LocalShadowResolution = 256;
inline constexpr UINT LocalShadowFaceMask = 4u;
enum class GDX12LocalShadowLevel { Off, Low, Medium, Manual, High, Epic };

struct GDX12LocalShadowProfile
{
    UINT Face, Page;
    const char* Name;
};
inline constexpr GDX12LocalShadowProfile LocalShadowProfiles[] = {
    {0, 0, "Off"}, {128, 512, "Low"}, {256, 1024, "Medium"},
    {384, 768, "Manual"}, {512, 2048, "High"}, {1024, 4096, "Epic"}
};
inline constexpr UINT LocalShadowAtlasCount = 4;
inline constexpr UINT MaxLocalShadowAtlasCount = D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION;
enum class ShadowCrossSync : std::uint8_t { Exact = 1, Latest = 3 };

inline std::wstring LocalShadowPageAdapterName(UINT page)
{
    return L"Render.Shadows.Local.Page" + std::to_wstring(page) + L".Adapter";
}

inline std::wstring LocalShadowPageCrossSyncName(UINT page)
{
    return L"Render.Shadows.Local.Page" + std::to_wstring(page) + L".CrossSync";
}

// The console registry treats names as case-insensitive.
inline std::optional<UINT> LocalShadowPageIndex(std::wstring_view name, std::wstring_view suffix)
{
    constexpr std::wstring_view prefix = L"Render.Shadows.Local.Page";
    if (name.size() <= prefix.size() + suffix.size() ||
        _wcsnicmp(name.data(), prefix.data(), prefix.size()) != 0 ||
        _wcsnicmp(name.data() + name.size() - suffix.size(), suffix.data(), suffix.size()) != 0) return {};
    const auto index = name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
    if (index.size() > 1 && index.front() == L'0') return {};
    UINT page = 0;
    for (wchar_t digit : index)
    {
        if (digit < L'0' || digit > L'9') return {};
        page = page * 10 + (digit - L'0');
        if (page >= MaxLocalShadowAtlasCount) return {};
    }
    return page;
}

inline std::optional<UINT> LocalShadowPageAdapterIndex(std::wstring_view name)
{
    return LocalShadowPageIndex(name, L".Adapter");
}

inline std::optional<UINT> LocalShadowPageCrossSyncIndex(std::wstring_view name)
{
    return LocalShadowPageIndex(name, L".CrossSync");
}

inline std::optional<UINT> LocalShadowPageSettingsIndex(std::wstring_view name)
{
    const auto adapter = LocalShadowPageAdapterIndex(name);
    return adapter ? adapter : LocalShadowPageCrossSyncIndex(name);
}

void RegisterLocalShadowPageSettings(UINT page);
void ReadLocalShadowPageSettings(std::vector<int>& adapters, std::vector<ShadowCrossSync>& sync, UINT pages);

constexpr GDX12LocalShadowProfile GetLocalShadowProfile(GDX12LocalShadowLevel level)
{
    return LocalShadowProfiles[static_cast<size_t>(level)];
}

constexpr GDX12LocalShadowProfile GetLocalShadowProfile(UINT face)
{
    for (const auto& profile : LocalShadowProfiles)
        if (profile.Face == face) return profile;
    return LocalShadowProfiles[0];
}

inline constexpr UINT SunShadowCascadeResolution = 2048;

struct GDX12SunCascadeSettings
{
    UINT Count = 4;
    float MaxDistance = 6000.0f;
    bool AutomaticSplits = true;
    XMFLOAT3 Splits = {0.067f, 0.2f, 0.467f};

    bool operator==(const GDX12SunCascadeSettings& other) const
    {
        return Count == other.Count && MaxDistance == other.MaxDistance && AutomaticSplits == other.AutomaticSplits &&
            Splits.x == other.Splits.x && Splits.y == other.Splits.y && Splits.z == other.Splits.z;
    }
};

struct GDX12ShadowQuality
{
    UINT SunResolution = SunShadowCascadeResolution;
    UINT LocalResolution = LocalShadowResolution;
    GDX12SunCascadeSettings SunCascades;
    UINT LocalAtlasCount = LocalShadowAtlasCount;

    bool operator==(const GDX12ShadowQuality& other) const
    { return SunResolution == other.SunResolution && LocalResolution == other.LocalResolution && LocalAtlasCount == other.LocalAtlasCount && SunCascades == other.SunCascades; }
};

constexpr bool IsSunShadowResolution(UINT value)
{
    return value == 512 || value == 1024 || value == 2048 || value == 4096;
}

constexpr bool IsLocalShadowResolution(UINT value)
{
    return GetLocalShadowProfile(value).Face != 0;
}

GDX12ShadowQuality ReadShadowQuality();

inline void ApplyShadowQuality(const GDX12ShadowQuality& quality, GDX12MainConstants& sun,
    std::vector<GDX12LocalLightConstants>& local)
{
    if (quality.SunResolution == 0) sun.SunShadowFlags = 0;
    if (quality.LocalResolution != 0) return;
    for (auto& light : local)
    {
        light.ShadowFlags = 0;
        light.ShadowFirstSlice = UINT_MAX;
    }
}

struct GDX12LocalShadowKey
{
    UINT EntityId = 0;
    uint64_t Generation = 0;

    bool operator==(const GDX12LocalShadowKey& other) const
    { return EntityId == other.EntityId && Generation == other.Generation; }
};

inline void SetLocalShadowGeneration(GDX12LocalLightConstants& light, uint64_t generation)
{
    // CPU cache identity uses two unused words of the existing GPU layout.
    light._padding[0] = static_cast<UINT>(generation);
    light._padding[1] = static_cast<UINT>(generation >> 32);
}

inline GDX12LocalShadowKey GetLocalShadowKey(const GDX12LocalLightConstants& light)
{
    return { light.EntityId, (static_cast<uint64_t>(light._padding[1]) << 32) | light._padding[0] };
}

struct GDX12ShadowRegion
{
    UINT Slice, X, Y, Width, Height;
};

inline GDX12ShadowRegion SunShadowRegion(UINT cascade, UINT resolution = SunShadowCascadeResolution)
{
    return { 0, (cascade & 1u) * resolution, (cascade >> 1u) * resolution, resolution, resolution };
}

inline GDX12ShadowRegion LocalShadowRegion(const GDX12LocalLightConstants& light, UINT face,
    UINT resolution = LocalShadowResolution)
{
    return { light.ShadowFirstSlice + face, 0, 0, resolution, resolution };
}

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

class GDX12LocalShadowFrustum
{
public:
    explicit GDX12LocalShadowFrustum(const XMFLOAT4X4& viewProjection)
    {
        // Uploaded matrices are transposed: their rows are the clip-space plane coefficients.
        const XMMATRIX m = XMLoadFloat4x4(&viewProjection);
        const XMVECTOR planes[] = { m.r[3] + m.r[0], m.r[3] - m.r[0], m.r[3] + m.r[1],
            m.r[3] - m.r[1], m.r[2], m.r[3] - m.r[2] };
        for (size_t i = 0; i < _planes.size(); ++i)
        {
            XMStoreFloat4(&_planes[i].Equation, planes[i]);
            XMStoreFloat3(&_planes[i].AbsoluteNormal, XMVectorAbs(planes[i]));
        }
    }
    bool Intersects(const BoundingBox& bounds) const
    {
        const auto& c = bounds.Center;
        const auto& e = bounds.Extents;
        for (const auto& plane : _planes)
        {
            const auto& p = plane.Equation;
            const auto& a = plane.AbsoluteNormal;
            if (((c.x * p.x + c.y * p.y) + c.z * p.z) + p.w +
                ((e.x * a.x + e.y * a.y) + e.z * a.z) < 0) return false;
        }
        return true;
    }
private:
    struct Plane { XMFLOAT4 Equation; XMFLOAT3 AbsoluteNormal; };
    std::array<Plane, 6> _planes;
};

inline bool LocalShadowFaceIntersects(const XMFLOAT4X4& viewProjection, const BoundingBox& bounds)
{
    return GDX12LocalShadowFrustum(viewProjection).Intersects(bounds);
}

inline bool LocalShadowRangeIntersects(const GDX12LocalLightConstants& light, const BoundingBox& bounds)
{
    const auto distance = [](float position, float center, float extent)
    {
        const float lo = center - extent, hi = center + extent;
        return position < lo ? position - lo : position > hi ? position - hi : 0.0f;
    };
    const float x = distance(light.Position.x, bounds.Center.x, bounds.Extents.x);
    const float y = distance(light.Position.y, bounds.Center.y, bounds.Extents.y);
    const float z = distance(light.Position.z, bounds.Center.z, bounds.Extents.z);
    return (x * x + y * y) + z * z <= light.Range * light.Range;
}

inline bool LocalShadowIntersects(const GDX12LocalLightConstants& light, const BoundingBox& bounds)
{
    return LocalShadowRangeIntersects(light, bounds) &&
        (light.Type == 1 || LocalShadowFaceIntersects(light.ShadowViewProj[0], bounds));
}

inline GDX12LocalShadowVisibility BuildLocalShadowVisibility(const GDX12LocalLightConstants& light,
    const std::vector<GDX12LocalShadowCaster>& casters, const GDX12LocalShadowVisibility* staticVisibility = nullptr)
{
    GDX12LocalShadowVisibility result;
    result.StaticCasters.fill(14695981039346656037ull);
    result.AllCasters = result.StaticCasters;
    if (staticVisibility) result = *staticVisibility;
    if (light.ShadowFirstSlice == UINT_MAX) return result;
    std::vector<GDX12LocalShadowFrustum> faces;
    faces.reserve(LocalShadowFaceCount(light));
    for (UINT face = 0; face < LocalShadowFaceCount(light); ++face)
        faces.emplace_back(light.ShadowViewProj[face]);
    const auto hash = [](uint64_t& signature, const auto& value)
    {
        const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
        for (size_t i = 0; i < sizeof(value); ++i) signature = (signature ^ bytes[i]) * 1099511628211ull;
    };
    // Group signatures so cached static contributions can precede the current dynamic set.
    for (bool dynamic : { false, true })
    for (const auto& caster : casters)
    {
        if (caster.Dynamic != dynamic || (staticVisibility && !dynamic)) continue;
        if (!LocalShadowRangeIntersects(light, caster.Bounds)) continue;
        uint64_t signature = caster.Signature;
        hash(signature, caster.Bounds.Center);
        hash(signature, caster.Bounds.Extents);
        hash(signature, caster.Dynamic);
        for (UINT face = 0; face < LocalShadowFaceCount(light); ++face)
        {
            if (!faces[face].Intersects(caster.Bounds)) continue;
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
    return !(GetLocalShadowKey(light) == GetLocalShadowKey(*cached)) || light.ShadowRevision != cached->ShadowRevision ||
        light.Type != cached->Type || light.ShadowFirstSlice != cached->ShadowFirstSlice ||
        std::memcmp(light.ShadowViewProj, cached->ShadowViewProj, sizeof(light.ShadowViewProj)) != 0;
}

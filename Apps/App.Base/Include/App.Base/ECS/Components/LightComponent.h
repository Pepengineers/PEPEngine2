#pragma once
#include <atomic>
#include <cstdint>
#include "Engine.Core/ECS/Component.h"
#include "Engine.RendererDX12/D3DHelpers.h"

enum class LightType : std::uint8_t { Directional, Point, Spot };
enum class ShadowUpdateMode : std::uint8_t { Cached, EveryFrame };

struct LightComponent : ComponentTag
{
    LightType Type = LightType::Point;
    Vector3 Color = { 1.0f, 0.73f, 0.45f };
    float Intensity = 120.0f; // Candela for local lights; radiance for the directional light.
    float Range = 600.0f;
    float InnerAngle = 20.0f; // Spot half-angles in degrees.
    float OuterAngle = 35.0f;
    bool Enabled = true;
    bool CastShadows = true;
    ShadowUpdateMode ShadowUpdate = ShadowUpdateMode::Cached;
    uint32_t ShadowRevision = 1;

    void InvalidateShadow() { ++ShadowRevision; }
    uint64_t GetShadowGeneration() const { return _shadowLifetime.Generation; }

private:
    struct ShadowLifetime
    {
        ShadowLifetime() = default;
        ShadowLifetime(const ShadowLifetime&) : ShadowLifetime() {}
        ShadowLifetime& operator=(const ShadowLifetime&) { return *this; }
        // Dense pool moves relocate an existing light.
        ShadowLifetime(ShadowLifetime&&) noexcept = default;
        ShadowLifetime& operator=(ShadowLifetime&&) noexcept = default;

        uint64_t Generation = NextGeneration.fetch_add(1, std::memory_order_relaxed);
        inline static std::atomic<uint64_t> NextGeneration = 1;
    };
    ShadowLifetime _shadowLifetime;
};

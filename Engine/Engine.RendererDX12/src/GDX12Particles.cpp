#include "Engine.RendererDX12/GDX12Particles.h"
#include "Common/ConsoleVariables.h"
#include "Engine.RendererDX12/GDX12Quality.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace
{
    static AutoConsoleVariable<bool> Enabled(L"Render.Particles.Enabled", true,
        L"Enable GPU particle components", EConsoleVariableFlags::Default);
    static AutoConsoleVariable<bool> DrawEnabled(L"Render.Particles.DrawEnabled", true,
        L"Draw particles without changing simulation", EConsoleVariableFlags::Default);
    static AutoConsoleVariable<bool> SimulationEnabled(L"Render.Particles.SimulationEnabled", true,
        L"Advance particles and spawn; false freezes simulation", EConsoleVariableFlags::Default);
    static AutoConsoleVariable<bool> AsyncCompute(L"Render.Particles.AsyncCompute", false,
        L"Run particle simulation on COMPUTE; DIRECT fallback when unavailable", EConsoleVariableFlags::Default);
    static AutoConsoleVariable<GDX12Quality> Quality(L"Render.Particles.Quality", GDX12Quality::Mid,
        L"GPU particles: 0 Off, 1 Low, 2 Mid, 3 High, 4 Epic", EConsoleVariableFlags::Default);

    bool InRange(const float value, const float low, const float high)
    {
        return std::isfinite(value) && value >= low && value <= high;
    }

    bool ValidVector(const Vector3& value, const float limit)
    {
        return InRange(value.x, -limit, limit) && InRange(value.y, -limit, limit) && InRange(value.z, -limit, limit);
    }

    bool ValidColor(const Vector4& value)
    {
        return InRange(value.x, 0, 100) && InRange(value.y, 0, 100) &&
            InRange(value.z, 0, 100) && InRange(value.w, 0, 1);
    }
}

GDX12ParticleSettings GetParticleSettings()
{
    const int quality = Quality.GetInt();
    return { Enabled.GetValue() && quality != 0, quality, DrawEnabled.GetValue(), SimulationEnabled.GetValue(), AsyncCompute.GetValue() };
}

bool CanOffloadParticleEmitter(const GDX12ParticleEmitterSettings& settings)
{
    return settings.SharedSimulation && !settings.DepthCollision;
}

bool IsValidParticleEmitterSettings(const GDX12ParticleEmitterSettings& s)
{
    return s.MaxParticles <= 1048576 && s.Substeps <= 16 && s.EdgeSamples <= 4 &&
        (s.MinPixelSize == -1 || InRange(s.MinPixelSize, 0, 64)) &&
        InRange(s.EmissionRate, 0, 10000000) && InRange(s.LifetimeMin, 0.001f, 3600) &&
        InRange(s.LifetimeMax, s.LifetimeMin, 3600) && InRange(s.SpeedMin, 0, 1000000) &&
        InRange(s.SpeedMax, s.SpeedMin, 1000000) && ValidVector(s.Direction, 1) &&
        s.Direction.LengthSquared() > 0.000001f && InRange(s.ConeAngleDegrees, 0, 180) &&
        static_cast<UINT>(s.Shape) <= static_cast<UINT>(ParticleShape::Sphere) &&
        ValidVector(s.ShapeExtents, 1000000) && s.ShapeExtents.x >= 0 && s.ShapeExtents.y >= 0 && s.ShapeExtents.z >= 0 &&
        static_cast<UINT>(s.Simulation) < static_cast<UINT>(ParticleSimulation::Count) &&
        InRange(s.VolumeHalfExtents.x, 0.01f, 1000000) && InRange(s.VolumeHalfExtents.y, 0.01f, 1000000) &&
        InRange(s.VolumeHalfExtents.z, 0.01f, 1000000) &&
        ValidVector(s.Acceleration, 1000000) && ValidVector(s.Wind, 1000000) && InRange(s.Drag, 0, 1000) &&
        InRange(s.SizeStart, 0, 1000000) && InRange(s.SizeEnd, 0, 1000000) &&
        InRange(s.SizeVariation, 0, 1) && InRange(s.RenderScale, 0.001f, 100) &&
        ValidColor(s.ColorStart) && ValidColor(s.ColorEnd) && InRange(s.AngularVelocity, -10000, 10000) &&
        InRange(s.SoftDistance, 0, 1000000) && InRange(s.MaxDistance, 1, 100000000) &&
        InRange(s.MaxDeltaTime, 0.001f, 1) && InRange(s.TimeScale, 0, 100) &&
        static_cast<UINT>(s.Blend) <= static_cast<UINT>(ParticleBlend::SortedAlpha) &&
        static_cast<UINT>(s.Geometry) <= static_cast<UINT>(ParticleGeometry::WaterDrop) &&
        !(s.Geometry == ParticleGeometry::Firefly && s.Blend == ParticleBlend::SortedAlpha) &&
        s.AtlasColumns >= 1 && s.AtlasColumns <= 256 && s.AtlasRows >= 1 && s.AtlasRows <= 256 &&
        InRange(s.FramesPerSecond, 0, 1000) && InRange(s.CollisionRadius, 0.01f, 1000000) &&
        InRange(s.CollisionRestitution, 0, 1) && InRange(s.CollisionFriction, 0, 1) &&
        InRange(s.CollisionThickness, 0, 1000000) &&
        ValidVector(s.CollisionBoxCenter, 1000000) && InRange(s.CollisionBoxYawDegrees, -360, 360) &&
        InRange(s.CollisionBoxHalfExtents.x, 0.01f, 1000000) &&
        InRange(s.CollisionBoxHalfExtents.y, 0.01f, 1000000) &&
        InRange(s.CollisionBoxHalfExtents.z, 0.01f, 1000000);
}

const std::array<GDX12ParticleMeshVertex, FireflyParticleVertexCount>& GetFireflyParticleMesh()
{
    static const auto vertices = []
    {
        std::array<GDX12ParticleMeshVertex, FireflyParticleVertexCount> result;
        UINT next = 0;
        const auto octahedron = [&](const Vector3 center, const Vector3 radii, const Vector4 color, const float wing)
        {
            const Vector3 axes[] = {{1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1}};
            const UINT faces[][3] = {{2,4,0}, {2,0,5}, {2,5,1}, {2,1,4}, {3,0,4}, {3,5,0}, {3,1,5}, {3,4,1}};
            for (const auto& face : faces)
            {
                const Vector3 a = center + axes[face[0]] * radii;
                const Vector3 b = center + axes[face[1]] * radii;
                const Vector3 c = center + axes[face[2]] * radii;
                Vector3 normal = (b - a).Cross(c - a); normal.Normalize();
                for (const auto& position : {a, b, c}) result[next++] = {position, normal, color, wing};
            }
        };
        octahedron({0,0,0}, {0.075f,0.085f,0.20f}, {0.035f,0.06f,0.025f,0}, 0);
        octahedron({0,0,-0.22f}, {0.10f,0.10f,0.16f}, {1.4f,4,0.2f,1}, 0);
        octahedron({0,0,0.25f}, {0.07f,0.07f,0.08f}, {0.025f,0.04f,0.02f,0}, 0);
        octahedron({-0.23f,0.025f,-0.015f}, {0.24f,0.025f,0.19f}, {0.2f,0.3f,0.35f,0}, -1);
        octahedron({0.23f,0.025f,-0.015f}, {0.24f,0.025f,0.19f}, {0.2f,0.3f,0.35f,0}, 1);
        return result;
    }();
    return vertices;
}

GDX12ParticleEmitterSettings ResolveParticleEmitterSettings(const GDX12ParticleEmitterSettings& author, const int quality)
{
    if (quality < 0 || quality > 4 || !IsValidParticleEmitterSettings(author))
        throw std::invalid_argument("Invalid particle quality or author settings");
    auto settings = author;
    const int index = (std::max)(1, quality) - 1;
    constexpr UINT capacities[] = { 16384, 65536, 131072, 262144 };
    constexpr UINT substeps[] = { 1, 2, 4, 8 };
    constexpr UINT samples[] = { 1, 2, 4, 4 };
    constexpr float minPixels[] = { 1.5f, 0.75f, 0.0f, 0.0f };
    if (!settings.MaxParticles) settings.MaxParticles = capacities[index];
    if (!settings.Substeps) settings.Substeps = substeps[index];
    if (!settings.EdgeSamples) settings.EdgeSamples = samples[index];
    if (settings.MinPixelSize == -1) settings.MinPixelSize = minPixels[index];
    return settings;
}

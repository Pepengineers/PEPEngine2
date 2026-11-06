#include "Rendering/Water/GDX12Fluid.h"
#include "Common/ConsoleVariables.h"
#include "Engine.RendererDX12/GDX12Quality.h"
#include <cmath>
#include <stdexcept>

namespace
{
    static AutoConsoleVariable<bool> Enabled(L"Render.Fluid.Enabled", true,
        L"Enable fluid components. Default true.", EConsoleVariableFlags::Default);
    static AutoConsoleVariable<GDX12Quality> Quality(L"Render.Fluid.Quality", GDX12Quality::Mid,
        L"0 Off, 1 Low, 2 Mid (default), 3 High.", EConsoleVariableFlags::Default);
    bool Finite(const Vector3 value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    }
    bool Range(const float value, const float low, const float high)
    {
        return std::isfinite(value) && value >= low && value <= high;
    }
}

GDX12FluidControls GetFluidControls()
{
    const int quality = Quality.GetInt();
    return { Enabled.GetValue() && quality != 0, quality };
}

GDX12FluidSettings ResolveFluidSettings(const GDX12FluidSettings& author, const int quality)
{
    if (quality < 0 || quality > 3) throw std::invalid_argument("Invalid fluid quality");
    auto result = author;
    const UINT level = static_cast<UINT>((std::max)(1, quality));
    if (!result.MaxSubsteps) result.MaxSubsteps = 16u << (level - 1);
    if (!result.FilterIterations) result.FilterIterations = level;
    if (!result.FilterRadiusPixels) result.FilterRadiusPixels = 8u << (level - 1);
    return result;
}

void ValidateFluidSettings(const GDX12FluidSettings& s)
{
    if (!Range(s.MaxStepSeconds, 0.000001f, 0.05f) || s.MaxSubsteps > 64 ||
        !Range(s.TimeScale, 0, 8) || !Range(s.SmoothingRadiusCm, 0.1f, 1000) ||
        !Range(s.ParticleRadiusCm, 0.01f, s.SmoothingRadiusCm) ||
        !Range(s.ParticleMassKg, 1e-9f, 100000) || !Range(s.RestDensityKgPerCm3, 1e-8f, 100) ||
        !Range(s.PressureStiffnessCm2PerSecond2, 1, 1e8f) ||
        !Range(s.ViscosityCm2PerSecond, 0, 100000) || !Finite(s.AccelerationCmPerSecond2) ||
        s.AccelerationCmPerSecond2.Length() > s.MaxAccelerationCmPerSecond2 ||
        !Range(s.MaxSpeedCmPerSecond, 1, 100000) || !Range(s.MaxAccelerationCmPerSecond2, 1, 1e8f) ||
        !Finite(s.ContainerHalfExtentCm) || s.ContainerHalfExtentCm.x <= s.ParticleRadiusCm ||
        s.ContainerHalfExtentCm.y <= s.ParticleRadiusCm || s.ContainerHalfExtentCm.z <= s.ParticleRadiusCm ||
        s.ContainerHalfExtentCm.x > 1e6f || s.ContainerHalfExtentCm.y > 1e6f || s.ContainerHalfExtentCm.z > 1e6f ||
        !Range(s.Restitution, 0, 1) || s.FilterIterations > 16 || s.FilterRadiusPixels > 64 ||
        !Range(s.FilterSigmaCm, 0.001f, 1000) || !Range(s.FilterDepthRangeCm, 0.001f, 100000) ||
        !Range(s.FilterEdgeCm, 0, s.FilterDepthRangeCm) || !Range(s.IOR, 1, 3) ||
        !Finite(s.AbsorptionPerCm) || !Range(s.AbsorptionPerCm.x, 0, 100) ||
        !Range(s.AbsorptionPerCm.y, 0, 100) || !Range(s.AbsorptionPerCm.z, 0, 100) ||
        !Finite(s.ScatteringColor) || !Range(s.ScatteringColor.x, 0, 100) ||
        !Range(s.ScatteringColor.y, 0, 100) || !Range(s.ScatteringColor.z, 0, 100) ||
        !Range(s.Roughness, 0, 1) || !Range(s.RefractionPixels, 0, 128) ||
        !Range(s.MaxOpticalThicknessCm, 0.01f, 1e6f))
        throw std::invalid_argument("Invalid fluid component settings");
}

void GDX12FluidComponent::SetSettings(const GDX12FluidSettings& settings)
{
    ValidateFluidSettings(settings);
    _settings = settings;
    ++_settingsRevision;
}

void GDX12FluidComponent::SetAcceleration(const Vector3 acceleration)
{
    auto settings = _settings;
    settings.AccelerationCmPerSecond2 = acceleration;
    ValidateFluidSettings(settings);
    _settings.AccelerationCmPerSecond2 = acceleration;
}

void GDX12FluidComponent::SetParticles(std::vector<GDX12FluidParticle> particles)
{
    if (particles.size() > 1048576) throw std::invalid_argument("Fluid seed exceeds 1048576 particles");
    for (const auto& particle : particles)
        if (!Finite(particle.Position) || !Finite(particle.Velocity) ||
            particle.Position.Length() > 1e7f || particle.Velocity.Length() > 1e7f)
            throw std::invalid_argument("Invalid fluid seed particle");
    _particles = std::make_shared<const std::vector<GDX12FluidParticle>>(std::move(particles));
    Reset();
}

void ValidateFluidInstance(const GDX12FluidInstance& instance)
{
    ValidateFluidSettings(instance.Component.GetSettings());
    if (!std::isfinite(instance.DeltaSeconds) || instance.DeltaSeconds < 0 || instance.DeltaSeconds > 3600)
        throw std::invalid_argument("Invalid fluid frame time");
    const auto& matrix = instance.ContainerToWorld;
    const auto* values = reinterpret_cast<const float*>(&matrix);
    for (UINT i = 0; i < 16; ++i)
        if (!std::isfinite(values[i]) || std::abs(values[i]) > 1e7f)
            throw std::invalid_argument("Invalid fluid container transform");
    const float determinant = matrix.Determinant();
    if (!std::isfinite(determinant) || std::abs(determinant) < 1e-12f ||
        matrix._14 != 0 || matrix._24 != 0 || matrix._34 != 0 || matrix._44 != 1)
        throw std::invalid_argument("Fluid container requires an invertible affine transform");
    const auto extent = instance.Component.GetSettings().ContainerHalfExtentCm;
    for (UINT corner = 0; corner < 8; ++corner)
    {
        const Vector3 local((corner & 1) ? extent.x : -extent.x,
            (corner & 2) ? extent.y : -extent.y, (corner & 4) ? extent.z : -extent.z);
        const auto world = Vector3::Transform(local, matrix);
        if (!Finite(world) || world.Length() > 1e7f)
            throw std::invalid_argument("Fluid container outside supported world range");
    }
    const Matrix inverse = matrix.Invert();
    const auto& s = instance.Component.GetSettings();
    const Vector3 margin = s.ParticleRadiusCm * Vector3(
        Vector3(inverse._11, inverse._21, inverse._31).Length(),
        Vector3(inverse._12, inverse._22, inverse._32).Length(),
        Vector3(inverse._13, inverse._23, inverse._33).Length());
    if (!Finite(margin) || margin.x >= s.ContainerHalfExtentCm.x ||
        margin.y >= s.ContainerHalfExtentCm.y || margin.z >= s.ContainerHalfExtentCm.z)
        throw std::invalid_argument("Fluid particles do not fit the transformed container");
}


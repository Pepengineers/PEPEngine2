#pragma once
#include "Engine.RendererDX12/D3DHelpers.h"
#include <cstdint>
#include <memory>
#include <vector>

struct GDX12FluidParticle
{
    Vector3 Position = Vector3::Zero;
    Vector3 Velocity = Vector3::Zero;
};

struct GDX12FluidSettings
{
    bool SimulationEnabled = true;
    bool RenderEnabled = true;
    // Keep the solver in container-local reference units; rendering converts to world cm.
    bool ReferenceModel = false;
    float MaxStepSeconds = 1.0f / 240.0f;
    UINT MaxSubsteps = 0;
    float TimeScale = 1;
    float SmoothingRadiusCm = 4;
    float ParticleRadiusCm = 1;
    float ParticleMassKg = 0.008f;
    float RestDensityKgPerCm3 = 0.001f;
    float PressureStiffnessCm2PerSecond2 = 40000;
    // Nonzero enables signed/near-pressure kernels; zero preserves the legacy solver.
    float NearPressureStiffnessCm2PerSecond2 = 0;
    float PredictionSeconds = 0;
    float ViscosityCm2PerSecond = 1;
    Vector3 AccelerationCmPerSecond2 = Vector3(0, -981, 0);
    float WaveAcceleration = 0;
    float WavePeriodSeconds = 6;
    float MaxSpeedCmPerSecond = 1000;
    float MaxAccelerationCmPerSecond2 = 100000;
    Vector3 ContainerHalfExtentCm = Vector3(100);
    float Restitution = 0.2f;
    UINT FilterIterations = 0;
    UINT FilterRadiusPixels = 0;
    float FilterSigmaCm = 0.7f;
    float FilterDepthRangeCm = 10;
    float FilterEdgeCm = 1;
    float IOR = 1.333f;
    Vector3 AbsorptionPerCm = Vector3(0.015f, 0.004f, 0.002f);
    Vector3 ScatteringColor = Vector3(0.02f, 0.08f, 0.1f);
    float Roughness = 0.05f;
    float RefractionPixels = 12;
    float MaxOpticalThicknessCm = 1000;
};

struct GDX12FluidControls
{
    bool Enabled;
    int Quality;
    bool AsyncCompute = false;

};
GDX12FluidControls GetFluidControls();
GDX12FluidSettings ResolveFluidSettings(const GDX12FluidSettings& author, int quality);
void ValidateFluidSettings(const GDX12FluidSettings& settings);

// Immutable seeds: container-local cm, or raw reference units when ReferenceModel is enabled.
class GDX12FluidComponent
{
public:
    const GDX12FluidSettings& GetSettings() const { return _settings; }
    void SetSettings(const GDX12FluidSettings& settings);
    void SetParticles(std::vector<GDX12FluidParticle> particles);
    const std::shared_ptr<const std::vector<GDX12FluidParticle>>& GetParticles() const { return _particles; }
    void Reset() { ++_resetRevision; }
    uint64_t GetResetRevision() const { return _resetRevision; }
    uint64_t GetSettingsRevision() const { return _settingsRevision; }
private:
    GDX12FluidSettings _settings;
    std::shared_ptr<const std::vector<GDX12FluidParticle>> _particles;
    uint64_t _resetRevision = 1;
    uint64_t _settingsRevision = 1;
};

struct GDX12FluidInstance
{
    uint64_t Id = 0;
    float DeltaSeconds = 0;
    float ClockScale = 1;
    GDX12FluidComponent Component;
    Matrix ContainerToWorld = Matrix::Identity;
};

void ValidateFluidInstance(const GDX12FluidInstance& instance);

#pragma once

// Uses the SecondGroupTests camera (near=1, far=10000) and its independent HDR/motion readback.
template<class Execute>
static void CheckParticleSimulationControls(GDX12Device& device, GDX12DeviceResources& resources,
    GDX12ParticlesPass& particles, Execute&& execute, Vector3 eye, Vector3 forward)
{
    GDX12ParticleEmitter emitter;
    emitter.Id = 901;
    emitter.MaxParticles = 16;
    emitter.BurstTotal = 1;
    emitter.EmissionRate = 0;
    emitter.World = Matrix::CreateTranslation(eye + forward * 100);
    emitter.SpeedMin = emitter.SpeedMax = 0;
    emitter.Acceleration = forward * 100;
    emitter.LifetimeMin = emitter.LifetimeMax = 10;
    emitter.SizeStart = emitter.SizeEnd = 10;
    emitter.SizeVariation = 0;
    emitter.Substeps = 1;
    emitter.SoftDistance = 0;
    emitter.ColorStart = emitter.ColorEnd = Vector4(0,0,0,1);
    emitter.Blend = ParticleBlend::Additive;
    const auto render = [&]
    {
        resources.ParticleEmitters = {emitter};
        execute(particles);
    };
    const auto distance = [&]
    {
        Require(resources.ParticleMotion != nullptr, "Particle simulation fixture produced no motion target");
        const auto alpha = Read(device, *resources.ParticleMotion, 3);
        const float weight = std::accumulate(alpha.begin(), alpha.end(), 0.0f);
        Require(weight > 0, "Particle simulation fixture has no visible particle");
        const auto depth = Read(device, *resources.ParticleMotion, 2);
        return std::accumulate(depth.begin(), depth.end(), 0.0f) / weight * 10000;
    };
    for (const auto mode : {ParticleSimulation::AnalyticDrag, ParticleSimulation::SemiImplicitEuler})
    {
        emitter.Simulation = mode;
        emitter.ResetRevision = 2000 + static_cast<UINT>(mode);
        emitter.DeltaTime = 0;
        render();
        Require(std::abs(distance() - 100) < 0.03f, "Particle did not spawn at the independent fixture origin");
        emitter.DeltaTime = 0.1f;
        render();
        // v0=0, a=100, dt=.1: trapezoidal position adds .5 cm, semi-implicit Euler adds 1 cm.
        const float expected = mode == ParticleSimulation::AnalyticDrag ? 100.5f : 101.0f;
        Require(std::abs(distance() - expected) < 0.03f, "GPU particle integrator differs from its independent displacement oracle");
    }
    emitter.ResetRevision = 2005;
    emitter.Simulation = ParticleSimulation::SemiImplicitEuler;
    emitter.Acceleration = Vector3::Zero;
    emitter.Direction = forward;
    emitter.ConeAngleDegrees = 0;
    emitter.SpeedMin = emitter.SpeedMax = 10;
    emitter.DeltaTime = 0;
    render();
    Set(L"Render.Particles.DrawEnabled", "false");
    emitter.DeltaTime = 0.1f;
    for (int frame = 0; frame < 3; ++frame)
    {
        render();
        Require(!resources.ParticleMotion, "Hidden particles published rendering output");
    }
    Set(L"Render.Particles.DrawEnabled", "true");
    Set(L"Render.Particles.SimulationEnabled", "false");
    render();
    Require(std::abs(distance() - 103) < 0.03f, "Hidden particle simulation stopped or restarted");
    render();
    Require(std::abs(distance() - 103) < 0.03f, "Disabled simulation continued moving");
    Set(L"Render.Particles.SimulationEnabled", "true");
    Set(L"Render.Particles.Enabled", "false");
    render();
    Require(!resources.ParticleMotion, "Master switch did not suppress rendering");
    Set(L"Render.Particles.SimulationEnabled", "false");
    Set(L"Render.Particles.Enabled", "true");
    render();
    Require(std::abs(distance() - 103) < 0.03f, "Master switch did not preserve the frozen pool");
    Set(L"Render.Particles.DrawEnabled", "false");
    ++emitter.ResetRevision;
    emitter.BurstTotal = 0;
    render();
    Require(!resources.ParticleMotion, "Hidden reset published rendering output");
    Set(L"Render.Particles.DrawEnabled", "true");
    render();
    const auto empty = Read(device, *resources.ParticleMotion, 3);
    Require(std::accumulate(empty.begin(), empty.end(), 0.0f) == 0, "Reset did not clear disabled simulation");
    emitter.BurstTotal = 1;
    render();
    const auto pending = Read(device, *resources.ParticleMotion, 3);
    Require(std::accumulate(pending.begin(), pending.end(), 0.0f) == 0, "Disabled simulation consumed a pending burst");
    Set(L"Render.Particles.SimulationEnabled", "true");
    emitter.DeltaTime = 0;
    render();
    Require(std::abs(distance() - 100) < 0.03f, "Resume lost the pending burst or caught up elapsed time");
    emitter.LifetimeMin = emitter.LifetimeMax = 0.15f;
    ++emitter.ResetRevision;
    render();
    Set(L"Render.Particles.SimulationEnabled", "false");
    emitter.DeltaTime = 0.1f;
    for (int frame = 0; frame < 3; ++frame) render();
    Require(std::abs(distance() - 100) < 0.03f, "Frozen particles aged or moved");
    Set(L"Render.Particles.SimulationEnabled", "true");
    Set(L"Render.Particles.DrawEnabled", "false");
    render();
    render();
    Set(L"Render.Particles.DrawEnabled", "true");
    emitter.DeltaTime = 0;
    render();
    const auto expired = Read(device, *resources.ParticleMotion, 3);
    Require(std::accumulate(expired.begin(), expired.end(), 0.0f) == 0, "Hidden particles did not expire");
    emitter.LifetimeMin = emitter.LifetimeMax = 10;
    emitter.World = Matrix::CreateRotationX(std::atan2(10.0f, 150.0f)) *
        Matrix::CreateTranslation(eye + forward * 100);
    emitter.Direction = Vector3::UnitZ;
    emitter.SpeedMin = emitter.SpeedMax = 100;
    emitter.Acceleration = Vector3::Zero;
    emitter.VolumeEnabled = true;
    emitter.VolumeHalfExtents = Vector3(100,100,1);
    emitter.CollisionRestitution = 1;
    emitter.ResetRevision = 2010;
    emitter.DeltaTime = 0;
    render();
    emitter.DeltaTime = 0.1f;
    render();
    Require(std::abs(distance() - 101) < 0.03f, "Rotated particle volume did not confine the first outward step");
    render();
    Require(std::abs(distance() - 99) < 0.03f, "Particle volume did not reflect velocity at its boundary");
    emitter.Paused = true;
    render();
    Require(std::abs(distance() - 99) < 0.03f, "Paused bounded particle continued simulating");
    std::cout << "GPU particle integrators, independent draw/simulation switches, pending burst/reset, volume and pause passed.\n";
}

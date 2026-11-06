#pragma once

// Uses the SecondGroupTests camera (near=1, far=10000) and its independent HDR/motion readback.
template<class Execute>
static void CheckParticleSimulationControls(GDX12Device& device, GDX12DeviceResources& resources,
    GDX12ParticlesPass& particles, Execute&& execute, GDX12Texture& sceneDepth, Vector3 eye, Vector3 forward)
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
    emitter.Paused = false;
    emitter.VolumeEnabled = false;
    emitter.DepthCollision = true;
    emitter.CollisionRadius = 2;
    emitter.CollisionRestitution = 0.5f;
    emitter.SizeStart = emitter.SizeEnd = 30;
    const auto cameraRight = forward.Cross(Vector3::UnitY);
    auto tangent = cameraRight;
    tangent.Normalize();
    const auto lateral = [&]
    {
        const auto coverage = Read(device, *resources.ParticleMotion, 3);
        const float weight = std::accumulate(coverage.begin(), coverage.end(), 0.0f);
        Require(weight > 0, "Particle depth-physics fixture has no visible particle");
        float x = 0;
        const UINT width = resources.ParticleMotion->GetWidth();
        for (size_t pixel = 0; pixel < coverage.size(); ++pixel)
            x += coverage[pixel] * (static_cast<float>(pixel % width) + 0.5f);
        return (2 * x / weight / width - 1) * distance() * width / resources.ParticleMotion->GetHeight();
    };
    for (const auto simulation : {ParticleSimulation::AnalyticDrag, ParticleSimulation::SemiImplicitEuler})
    {
        emitter.Simulation = simulation;
        emitter.Acceleration = Vector3::Zero;
        for (const float friction : {0.0f, 0.2f, 1.0f})
        {
            emitter.World = Matrix::CreateTranslation(eye + forward * 100);
            emitter.Direction = forward + tangent;
            emitter.Direction.Normalize();
            emitter.SpeedMin = emitter.SpeedMax = std::sqrt(2000000.0f);
            emitter.CollisionFriction = friction;
            ++emitter.ResetRevision;
            emitter.DeltaTime = 0;
            render();
            emitter.DeltaTime = 0.1f;
            render();
            // Hit at .048 s: normal speed 1000 -> -500; tangential impulse is mu * 1500.
            const float tangentSpeed = (std::max)(0.0f, 1000 - friction * 1500);
            Require(std::abs(distance() - 121.99f) < 0.03f,
                "Depth collision discarded the time remaining after impact");
            Require(std::abs(lateral() - (48 + tangentSpeed * 0.052f)) < 0.8f,
                "Depth collision did not apply the independent Coulomb friction impulse");
        }
        emitter.World = Matrix::CreateTranslation(eye + forward * 147.99f);
        emitter.SpeedMin = emitter.SpeedMax = 0;
        emitter.Acceleration = forward * 100;
        emitter.CollisionFriction = 0.2f;
        ++emitter.ResetRevision;
        emitter.DeltaTime = 0;
        render();
        emitter.DeltaTime = 0.1f;
        for (int frame = 0; frame < 20; ++frame)
        {
            render();
            Require(std::abs(distance() - 147.99f) < 0.03f,
                "Particle resting on scene depth gained bounce energy from gravity");
        }
    }
    emitter.Acceleration = Vector3::Zero;
    emitter.CollisionFriction = 0;
    emitter.CollisionRestitution = 1;
    emitter.SizeStart = emitter.SizeEnd = 10;
    std::cout << "GPU depth physics: remaining impact time, friction 0/.2/1 and resting gravity contact passed.\n";
    emitter.CollisionBoxEnabled = true;
    emitter.CollisionBoxCenter = eye + forward * 120;
    emitter.CollisionBoxHalfExtents = Vector3(100, 100, 1);
    emitter.CollisionBoxYawDegrees = 28.5f;
    emitter.World = Matrix::CreateTranslation(eye + forward * 100);
    emitter.Direction = forward;
    emitter.SpeedMin = emitter.SpeedMax = 1000;
    ++emitter.ResetRevision;
    emitter.DeltaTime = 0;
    render();
    emitter.DeltaTime = 0.1f;
    render();
    Require(distance() > 60 && distance() < 80,
        "Farther scene depth overrode the first impact against the door collider");
    auto* queue = device.GetCommandQueue();
    auto* commands = queue->GetCommandList();
    commands->ResourceBarrier({sceneDepth.GetResource()->GetDepthWriteBarrier()});
    commands->GetCommandList()->ClearDepthStencilView(sceneDepth.GetDSV()->CPUHandle, D3D12_CLEAR_FLAG_DEPTH,
        0, 0, 0, nullptr);
    queue->ExecuteCommandList(commands); queue->Flush();
    emitter.Paused = false;
    emitter.VolumeEnabled = false;
    emitter.DepthCollision = true;
    emitter.CollisionBoxEnabled = true;
    emitter.CollisionBoxCenter = eye + forward * 120;
    emitter.CollisionBoxHalfExtents = Vector3(100, 100, 1);
    emitter.CollisionBoxYawDegrees = 28.5f;
    emitter.CollisionRadius = 2;
    const auto normal = Vector3::TransformNormal(Vector3::UnitZ,
        Matrix::CreateRotationY(XMConvertToRadians(28.5f)));
    const float contactOffset = 3 / forward.Dot(normal);
    const float reboundSpeed = 1000 * (2 * forward.Dot(normal) * forward.Dot(normal) - 1);
    for (const float side : {-1.0f, 1.0f})
    {
        emitter.World = Matrix::CreateTranslation(eye + forward * (120 + side * 20));
        emitter.Direction = forward * -side;
        emitter.SpeedMin = emitter.SpeedMax = 1000;
        ++emitter.ResetRevision;
        emitter.DeltaTime = 0;
        render();
        emitter.DeltaTime = 0.1f; // Cross the entire thin obstacle in one substep, with no scene depth.
        render();
        const float remaining = 0.1f - (20 - contactOffset) / 1000;
        Require(std::abs(distance() - (120 + side * (contactOffset + 0.01f * forward.Dot(normal) +
            reboundSpeed * remaining))) < 0.03f,
            "Particle door collision did not continue the reflected motion for the remaining impact time");
        const float contact = distance();
        emitter.DeltaTime = 0.01f;
        render();
        Require((distance() - contact) * side > 4,
            "Particle did not bounce away from the door on both sides");
    }
    emitter.CollisionBoxEnabled = false;
    ++emitter.ResetRevision;
    emitter.DeltaTime = 0;
    render();
    emitter.DeltaTime = 0.1f;
    render();
    Require(std::abs(distance() - 40) < 0.03f, "Disabled door collider still blocked particles");
    emitter.CollisionBoxEnabled = true;
    emitter.SpeedMin = emitter.SpeedMax = 0;
    emitter.World = Matrix::CreateTranslation(emitter.CollisionBoxCenter + normal * 2);
    ++emitter.ResetRevision;
    emitter.DeltaTime = 0;
    render();
    emitter.DeltaTime = 0.1f;
    render();
    Require(std::abs(distance() - (120 + 3.01f * forward.Dot(normal))) < 0.03f,
        "Particle initially overlapping the door was not pushed out");
    const auto right = Vector3::TransformNormal(Vector3::UnitX,
        Matrix::CreateRotationY(XMConvertToRadians(28.5f)));
    emitter.CollisionBoxHalfExtents.x = 4;
    emitter.World = Matrix::CreateTranslation(emitter.CollisionBoxCenter + right * 10 - normal * 20);
    emitter.Direction = normal;
    emitter.SpeedMin = emitter.SpeedMax = 1000;
    ++emitter.ResetRevision;
    emitter.DeltaTime = 0;
    render();
    emitter.DeltaTime = 0.1f;
    render();
    Require(std::abs(distance() - (120 + 10 * forward.Dot(right) + 80 * forward.Dot(normal))) < 0.03f,
        "Particle missing the finite door footprint was incorrectly blocked");
    std::cout << "GPU particle swept rotated door collider: both sides, depth absent and disabled control passed.\n";
    std::cout << "GPU particle integrators, independent draw/simulation switches, pending burst/reset, volume and pause passed.\n";
}

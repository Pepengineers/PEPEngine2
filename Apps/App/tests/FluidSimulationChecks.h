#pragma once

template<class Execute>
static void CheckFluidSimulation(GDX12Device& device, GDX12FluidSurfacePass& water, Execute&& execute)
{
    struct Particle { Vector3 Position, Velocity, PreviousPosition; };
    const auto read = [&](GDX12Resource& resource)
    {
        const auto bytes = resource.D3DResource->GetDesc().Width;
        const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
        const auto desc = CD3DX12_RESOURCE_DESC::Buffer(bytes);
        ComPtr<ID3D12Resource> buffer;
        ThrowIfFailed(device.GetDevice()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&buffer)));
        auto* queue = device.GetCommandQueue();
        auto* commands = queue->GetCommandList();
        const auto state = resource.GetCurrentState();
        commands->ResourceBarrier({resource.GetCopySourceBarrier()});
        commands->GetCommandList()->CopyResource(buffer.Get(), resource.D3DResource.Get());
        commands->ResourceBarrier({resource.GetTransitionBarrier(state)});
        queue->ExecuteCommandList(commands); queue->Flush();
        void* mapped = nullptr;
        ThrowIfFailed(buffer->Map(0, nullptr, &mapped));
        std::vector<Particle> result(static_cast<size_t>(bytes) / sizeof(Particle));
        std::memcpy(result.data(), mapped, static_cast<size_t>(bytes));
        buffer->Unmap(0, nullptr);
        return result;
    };
    GDX12FluidInstance fluid;
    fluid.Id = 987;
    fluid.DeltaSeconds = 0.001f;
    GDX12FluidSettings settings;
    settings.RenderEnabled = false;
    settings.MaxStepSeconds = 0.001f;
    settings.MaxSubsteps = 1;
    settings.SmoothingRadiusCm = 20;
    settings.ParticleMassKg = 0.25f;
    settings.AccelerationCmPerSecond2 = Vector3::Zero;
    settings.PressureStiffnessCm2PerSecond2 = 28800;
    settings.ViscosityCm2PerSecond = 0;
    settings.PredictionSeconds = 1.0f / 120;
    for (const float nearStiffness : {0.0f, 215.0f})
    {
        settings.NearPressureStiffnessCm2PerSecond2 = nearStiffness;
        fluid.Component.SetSettings(settings);
        fluid.Component.SetParticles({{{-4,0,0},{}},{{4,0,0},{}}});
        water.SetInstances({fluid}); execute();
        float gap = 8;
        UINT buffers = 0;
        for (const auto& access : water.GetSimulationBufferAccesses())
        {
            if (access.Usage != GDX12RenderGraph::Mode::ReadWrite ||
                access.Resource->D3DResource->GetDesc().Width != 2 * sizeof(Particle)) continue;
            const auto particles = read(*access.Resource);
            gap = (std::min)(gap, particles[1].Position.x - particles[0].Position.x);
            for (const auto& p : particles)
                Require(std::isfinite(p.Position.x) && std::isfinite(p.Velocity.x), "Nonfinite fluid state");
            ++buffers;
        }
        Require(buffers == 2, "Fluid graph must expose both physical particle buffers");
        Require(nearStiffness ? gap < 7.99f : std::abs(gap - 8) < 1e-5f,
            "MGPU signed pressure must attract under-density neighbours; legacy SPH must stay unchanged");
        settings.SimulationEnabled = false;
        fluid.Component.SetSettings(settings); water.SetInstances({fluid}); execute();
        float pausedGap = 8;
        for (const auto& access : water.GetSimulationBufferAccesses())
            if (access.Usage == GDX12RenderGraph::Mode::ReadWrite &&
                access.Resource->D3DResource->GetDesc().Width == 2 * sizeof(Particle))
            {
                const auto particles = read(*access.Resource);
                pausedGap = (std::min)(pausedGap, particles[1].Position.x - particles[0].Position.x);
            }
        Require(pausedGap == gap, "Pausing fluid simulation must freeze its actual GPU particle positions");
        settings.SimulationEnabled = true;
    }
    settings.NearPressureStiffnessCm2PerSecond2 = 215;
    float unpredictedSpeed = 0;
    for (const float horizon : {0.0f, 1.0f / 120})
    {
        settings.PredictionSeconds = horizon;
        fluid.Component.SetSettings(settings);
        fluid.Component.SetParticles({{{-4,0,0},{-1000,0,0}},{{4,0,0},{1000,0,0}}});
        water.SetInstances({fluid}); execute();
        float speed = 1000;
        for (const auto& access : water.GetSimulationBufferAccesses())
            if (access.Usage == GDX12RenderGraph::Mode::ReadWrite &&
                access.Resource->D3DResource->GetDesc().Width == 2 * sizeof(Particle))
                speed = (std::min)(speed, std::abs(read(*access.Resource).front().Velocity.x));
        if (horizon == 0) unpredictedSpeed = speed;
        else Require(speed > unpredictedSpeed + 1,
            "Predicted neighbour positions must affect actual MGPU pressure forces");
    }
    settings.Restitution = 0.95f;
    fluid.Component.SetSettings(settings);
    fluid.Component.SetParticles({{{98.9f,0,0},{200,0,0}}});
    water.SetInstances({fluid}); execute();
    bool bounced = false;
    for (const auto& access : water.GetSimulationBufferAccesses())
        if (access.Usage == GDX12RenderGraph::Mode::ReadWrite &&
            access.Resource->D3DResource->GetDesc().Width == sizeof(Particle))
        {
            const auto p = read(*access.Resource).front();
            bounced |= p.Velocity.x < -189 && p.Velocity.x > -191 && p.Position.x <= 99;
        }
    Require(bounced, "MGPU container collision must retain 95 percent of the outgoing speed");
    settings.MaxStepSeconds = 1.0f / 180;
    settings.MaxSubsteps = 3;
    settings.SmoothingRadiusCm = 4;
    settings.ParticleMassKg = 0.0128f;
    settings.PressureStiffnessCm2PerSecond2 = 576000;
    settings.NearPressureStiffnessCm2PerSecond2 = 4300;
    settings.AccelerationCmPerSecond2 = Vector3(0,-981,0);
    fluid.DeltaSeconds = 0.1f;
    fluid.Component.SetSettings(settings);
    fluid.Component.SetParticles({{{0,0,0},{}}});
    water.SetInstances({fluid}); execute();
    bool stepped = false;
    for (const auto& access : water.GetSimulationBufferAccesses())
        if (access.Usage == GDX12RenderGraph::Mode::ReadWrite &&
            access.Resource->D3DResource->GetDesc().Width == sizeof(Particle))
        {
            const auto p = read(*access.Resource).front();
            stepped |= p.Velocity.y < -16 && p.Velocity.y > -16.5f &&
                p.Position.y < -0.17f && p.Position.y > -0.2f;
        }
    Require(stepped, "MGPU must advance three authored substeps, capped at 1/60 second per frame");
    fluid.DeltaSeconds = 1.0f / 240;
    fluid.Component.SetParticles({{{0,0,0},{}}});
    water.SetInstances({fluid}); execute();
    bool fastStepped = false;
    for (const auto& access : water.GetSimulationBufferAccesses())
        if (access.Usage == GDX12RenderGraph::Mode::ReadWrite &&
            access.Resource->D3DResource->GetDesc().Width == sizeof(Particle))
        {
            const auto p = read(*access.Resource).front();
            fastStepped |= std::abs(p.Position.y + 0.01134f) < 0.00005f;
        }
    Require(fastStepped, "MGPU must retain all three substeps at high frame rates");
    // Identical nonzero-dt state must survive DIRECT/COMPUTE/DIRECT switches bit-for-bit.
    settings.RenderEnabled = true;
    settings.MaxSubsteps = 3;
    fluid.DeltaSeconds = 1.0f / 60;
    fluid.Component.SetSettings(settings);
    fluid.Component.SetParticles({{{-2,0,0},{20,0,0}},{{2,0,0},{-20,0,0}}});
    std::vector<std::vector<Particle>> reference;
    for (bool async : {false, true, false})
    {
        Require(ConsoleModule{}.FindConsoleVariable(L"Render.Fluid.AsyncCompute").SetFromString(async ? "true" : "false"),
            "Cannot toggle fluid async in parity fixture");
        fluid.Component.Reset(); water.SetInstances({fluid});
        for (UINT frame = 0; frame < 4; ++frame) execute();
        std::vector<std::vector<Particle>> samples;
        for (const auto& access : water.GetSimulationBufferAccesses())
            if (access.Usage == GDX12RenderGraph::Mode::ReadWrite &&
                access.Resource->D3DResource->GetDesc().Width == 2 * sizeof(Particle))
                samples.push_back(read(*access.Resource));
        Require(samples.size() == 2, "Fluid parity fixture lost ping-pong buffers");
        if (reference.empty()) reference = samples;
        for (size_t i = 0; i < samples.size(); ++i)
            Require(std::memcmp(samples[i].data(), reference[i].data(), 2 * sizeof(Particle)) == 0,
                "DIRECT/COMPUTE fluid solver state differs with identical nonzero dt and reset");
        fluid.DeltaSeconds = 0; water.SetInstances({fluid}); execute();
        for (const auto& access : water.GetSimulationBufferAccesses())
            if (access.Usage == GDX12RenderGraph::Mode::ReadWrite &&
                access.Resource->D3DResource->GetDesc().Width == 2 * sizeof(Particle))
            {
                const auto particles = read(*access.Resource);
                for (const auto& particle : particles)
                    Require(std::isfinite(particle.PreviousPosition.y), "Zero-dt fluid motion history became nonfinite");
                // Four three-substep frames leave buffer zero current; SavePrevious must still run.
                Require(std::memcmp(&particles[0].Position, &particles[0].PreviousPosition, sizeof(Vector3)) == 0,
                    "Zero-dt fluid failed to save its displayed previous position");
                break;
            }
        fluid.DeltaSeconds = 1.0f / 60;
    }
    fluid.Component.Reset();
    for (bool async : {false, true, false, true})
    {
        Require(ConsoleModule{}.FindConsoleVariable(L"Render.Fluid.AsyncCompute").SetFromString(async ? "true" : "false"),
            "Cannot toggle a running fluid simulation");
        water.SetInstances({fluid}); execute();
    }
    size_t buffer = 0;
    for (const auto& access : water.GetSimulationBufferAccesses())
        if (access.Usage == GDX12RenderGraph::Mode::ReadWrite &&
            access.Resource->D3DResource->GetDesc().Width == 2 * sizeof(Particle))
        {
            const auto particles = read(*access.Resource);
            Require(buffer < reference.size() && std::memcmp(particles.data(), reference[buffer].data(), 2 * sizeof(Particle)) == 0,
                "Queue toggles without reset changed the running fluid state");
            ++buffer;
        }
    Require(buffer == 2, "Running fluid toggle fixture lost its buffers");
    settings.MGPUReference = true;
    settings.RenderEnabled = false;
    settings.MaxSubsteps = 1;
    settings.MaxStepSeconds = 0.001f;
    settings.ParticleRadiusCm = 0.1f;
    settings.SmoothingRadiusCm = 0.2f;
    settings.ParticleMassKg = 1;
    settings.RestDensityKgPerCm3 = 630;
    settings.PressureStiffnessCm2PerSecond2 = 288;
    settings.NearPressureStiffnessCm2PerSecond2 = 2.15f;
    settings.AccelerationCmPerSecond2 = Vector3::Zero;
    settings.ContainerHalfExtentCm = Vector3(13, 1.9f, 13.2f);
    settings.MaxSpeedCmPerSecond = 1;
    settings.MaxAccelerationCmPerSecond2 = 1;
    fluid.DeltaSeconds = 0.001f;
    fluid.ContainerToWorld = Matrix::CreateScale(20) * Matrix::CreateRotationY(0.7f) * Matrix::CreateTranslation(-1831,65,-989);
    fluid.Component.SetSettings(settings);
    fluid.Component.SetParticles({{{12.99f,0,0},{100,0,0}}});
    water.SetInstances({fluid}); execute();
    bool referenceBounce = false;
    for (const auto& access : water.GetSimulationBufferAccesses())
        if (access.Usage == GDX12RenderGraph::Mode::ReadWrite &&
            access.Resource->D3DResource->GetDesc().Width == sizeof(Particle))
        {
            const auto p = read(*access.Resource).front();
            referenceBounce |= std::abs(p.Position.x - 13) < 1e-5f && std::abs(p.Velocity.x + 94.92875f) < 0.001f;
        }
    Require(referenceBounce, "Raw MGPU units must ignore render scale, speed caps and sphere collision margins");
    fluid.Component.SetParticles({{{-.05f,0,0},{}},{{.05f,0,0},{}}});
    water.SetInstances({fluid}); execute();
    const float pi = 3.14159265359f, h = .2f, q = .1f;
    const float density = 15 / (2 * pi * std::pow(h,5.0f)) * (h*h + q*q);
    const float nearDensity = 15 / (pi * std::pow(h,6.0f)) * (h*h*h + q*q*q);
    const float acceleration = ((density - 630) * 288 * (-15 / (pi * std::pow(h,5.0f)) * q) / density +
        nearDensity * 2.15f * (-45 / (pi * std::pow(h,6.0f)) * q*q) / nearDensity) / density;
    const float expectedVelocity = acceleration * .001f * (1 - .75f * .001f);
    bool referencePressure = false;
    for (const auto& access : water.GetSimulationBufferAccesses())
        if (access.Usage == GDX12RenderGraph::Mode::ReadWrite &&
            access.Resource->D3DResource->GetDesc().Width == 2 * sizeof(Particle))
            referencePressure |= std::abs(read(*access.Resource).front().Velocity.x - expectedVelocity) < 0.002f;
    Require(referencePressure && expectedVelocity > 0, "GPU pressure must match unscaled MGPU equations, without an acceleration cap");
    water.SetInstances({}); execute();
    std::cout << "GPU fluid: MGPU cohesion/prediction, pause, 95-percent rebound, fixed substeps and legacy pressure isolation passed.\n";
}

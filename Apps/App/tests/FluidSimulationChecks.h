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
        for (const auto& access : water.GetBufferAccesses())
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
        for (const auto& access : water.GetBufferAccesses())
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
        for (const auto& access : water.GetBufferAccesses())
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
    for (const auto& access : water.GetBufferAccesses())
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
    for (const auto& access : water.GetBufferAccesses())
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
    for (const auto& access : water.GetBufferAccesses())
        if (access.Usage == GDX12RenderGraph::Mode::ReadWrite &&
            access.Resource->D3DResource->GetDesc().Width == sizeof(Particle))
        {
            const auto p = read(*access.Resource).front();
            fastStepped |= std::abs(p.Position.y + 0.01134f) < 0.00005f;
        }
    Require(fastStepped, "MGPU must retain all three substeps at high frame rates");
    water.SetInstances({}); execute();
    std::cout << "GPU fluid: MGPU cohesion/prediction, pause, 95-percent rebound, fixed substeps and legacy pressure isolation passed.\n";
}

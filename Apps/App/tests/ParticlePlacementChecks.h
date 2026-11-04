#pragma once

static void BenchmarkParticlePlacement(GDX12Device& device, GDX12DeviceResources& resources,
    RenderPipelineCommonData& common, GDX12Texture* depth, GDX12Texture* hdr,
    GDX12Texture* accum, GDX12Texture* reveal)
{
    auto* queue = device.GetCommandQueue();
    const UINT previousSlot = resources.CurrFrameConstantsIndex;
    auto previousEmitters = std::move(resources.ParticleEmitters);
    auto* previousMotion = resources.ParticleMotion;
    auto previousSimulation = std::move(resources.ParticleSimulation);
    resources.ParticleSimulation = std::make_unique<GDX12ParticleSimulation>();
    GDX12ParticlesPass particles;
    particles.Setup(&resources, nullptr, &common);
    particles.SetInputs({depth, accum, reveal});
    particles.Initialize();
    GDX12ParticleEmitter emitter;
    emitter.Id = 149;
    emitter.MaxParticles = 1024;
    emitter.BurstTotal = emitter.MaxParticles;
    emitter.EmissionRate = 0;
    emitter.Geometry = ParticleGeometry::Firefly;
    emitter.Shape = ParticleShape::Box;
    emitter.ShapeExtents = Vector3(60, 25, 30);
    emitter.World = Matrix::CreateTranslation(0, 30, -40);
    emitter.SpeedMin = emitter.SpeedMax = 0;
    emitter.Acceleration = Vector3::Zero;
    emitter.SizeStart = emitter.SizeEnd = 4;
    emitter.SizeVariation = 0;
    emitter.ColorStart = emitter.ColorEnd = Vector4(0.01f, 0.01f, 0.01f, 1);
    emitter.DeltaTime = 0;

    D3D12_QUERY_HEAP_DESC queryDesc = {};
    queryDesc.Type = D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS;
    queryDesc.Count = 1;
    ComPtr<ID3D12QueryHeap> query;
    ThrowIfFailed(device.GetDevice()->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&query)));
    auto statistics = device.CreateResource(D3D12_HEAP_TYPE_READBACK,
        CD3DX12_RESOURCE_DESC::Buffer(sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS)), D3D12_RESOURCE_STATE_COPY_DEST);
    for (auto blend : {ParticleBlend::WeightedAlpha, ParticleBlend::Additive})
    {
        emitter.Blend = blend;
        ++emitter.ResetRevision;
        resources.ParticleEmitters = {emitter};
        resources.ParticleSimulation->BeginFrame(&resources, common);
        particles.BeginFrame();
        const auto record = [&](GDX12CommandList* commands, UINT slot, UINT)
        {
            resources.CurrFrameConstantsIndex = slot;
            resources.ParticleSimulation->BeginFrame(&resources, common);
            particles.BeginFrame();
            commands->ResourceBarrier({depth->GetResource()->GetDepthWriteBarrier(),
                hdr->GetResource()->GetRenderTargetBarrier(), accum->GetResource()->GetRenderTargetBarrier(),
                reveal->GetResource()->GetRenderTargetBarrier()});
            commands->GetCommandList()->ClearDepthStencilView(depth->GetDSV()->CPUHandle,
                D3D12_CLEAR_FLAG_DEPTH, 0, 0, 0, nullptr);
            for (auto* texture : {hdr, accum, reveal}) commands->ClearRenderTargetView(texture);
            resources.ParticleSimulation->InitializePools(commands, &resources);
            resources.ParticleSimulation->Execute(commands, &resources, common, depth);
            particles.Execute(commands);
            particles.Composite(commands, hdr);
        };
        auto* commands = queue->GetCommandList();
        commands->GetCommandList()->BeginQuery(query.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
        record(commands, 0, 0);
        commands->GetCommandList()->EndQuery(query.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
        commands->GetCommandList()->ResolveQueryData(query.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0, 1,
            statistics.Get(), 0);
        queue->ExecuteCommandList(commands); queue->Flush();
        void* mapped = nullptr;
        const D3D12_RANGE range = {0, sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS)};
        ThrowIfFailed(statistics->Map(0, &range, &mapped));
        const auto stats = *static_cast<D3D12_QUERY_DATA_PIPELINE_STATISTICS*>(mapped);
        const D3D12_RANGE noWrite = {};
        statistics->Unmap(0, &noWrite);
        Require(stats.GSInvocations == 0 && stats.IAPrimitives == UINT64(emitter.MaxParticles) * 80,
            "Particle placement benchmark must draw every mesh particle in color and motion");
        Require(resources.ParticleMotion, "Particle placement benchmark has no motion output");
        const auto coverage = Read(device, *resources.ParticleMotion, 3);
        Require(*std::max_element(coverage.begin(), coverage.end()) > 0,
            "Particle placement benchmark has no visible mesh particles");
        MeasurePlacement(device, blend == ParticleBlend::WeightedAlpha ? "mesh-particles-weighted-1024" :
            "mesh-particles-additive-1024", static_cast<UINT>(resources.FrameConstants.size()), record);
    }
    queue->Flush();
    particles.ClearDenendencies();
    resources.ParticleSimulation = std::move(previousSimulation);
    resources.ParticleMotion = previousMotion;
    resources.ParticleEmitters = std::move(previousEmitters);
    resources.CurrFrameConstantsIndex = previousSlot;
}

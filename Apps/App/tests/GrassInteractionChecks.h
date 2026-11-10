#pragma once
#include "../src/BistroArch.h"
#include <fstream>

// Read the actual GPU buffers, independently of rasterization, LOD and blend ordering.
template<class T>
static std::vector<T> ReadInteractionBuffer(GDX12Device& device, GDX12Resource& source, UINT count)
{
    auto target = device.CreateResource(D3D12_HEAP_TYPE_READBACK,
        CD3DX12_RESOURCE_DESC::Buffer(UINT64(count) * sizeof(T)), D3D12_RESOURCE_STATE_COPY_DEST);
    auto* queue = device.GetCommandQueue();
    auto* commands = queue->GetCommandList();
    const auto priorState = source.GetCurrentState();
    commands->ResourceBarrier({ source.GetCopySourceBarrier() });
    commands->GetCommandList()->CopyBufferRegion(target.Get(), 0, source.D3DResource.Get(), 0, UINT64(count) * sizeof(T));
    commands->ResourceBarrier({ source.GetTransitionBarrier(priorState) });
    queue->ExecuteCommandList(commands); queue->Flush();
    void* mapped = nullptr;
    ThrowIfFailed(target->Map(0, nullptr, &mapped));
    std::vector<T> values(count);
    std::memcpy(values.data(), mapped, size_t(count) * sizeof(T));
    target->Unmap(0, nullptr);
    return values;
}

class InteractionPreparation final : public GDX12RenderPass
{
public:
    explicit InteractionPreparation(GDX12GrassRenderer& renderer) : Renderer(renderer) {}
    void RecordRenderGraph(GDX12RenderGraphBuilder& graph) override
    { graph.SetName("Interaction test preparation"); graph.AsyncCompute(); graph.HasSideEffects(); }
    void Execute(GDX12CommandList* commands) override { Renderer.PrepareGPU(commands); }
    std::vector<GDX12RenderGraph::BufferAccess> GetBufferAccesses() const override { return Renderer.PreparationAccesses(); }
    GDX12GrassRenderer& Renderer;
};

static void CheckGrassLatestHandoff(const GDX12GrassFieldProducer::SnapshotLayout& layout)
{
    using namespace GDX12Submission;
    using Selection = GDX12GrassLatestSelection;
    const auto stream = static_cast<TransferId>(1);
    const auto epoch = static_cast<Epoch>(2);
    const auto version = [&](uint64_t invocation)
    {
        return TransferVersion{ stream, epoch, static_cast<ProducerInvocationId>(invocation) };
    };

    auto seed = layout;
    seed.InteractionTimeSeconds = 4;
    seed.InteractionRevision = 240;
    for (auto& wind : seed.Wind) wind.TimeSeconds = 3;
    const auto capture = std::chrono::steady_clock::time_point{} + std::chrono::seconds(10);
    Selection selection(stream, epoch, { 240, seed, capture });

    Require(!selection.TryAdopt(version(1), { 120, layout, capture }) && selection.Displayed().Sequence == 240,
        "First Latest adoption rewound the displayed primary tick");
    Require(!selection.TryAdopt(version(1), { 241, layout, capture }) && selection.Displayed().Layout.InteractionTimeSeconds == 4,
        "New sequence concealed a restarted secondary interaction clock");
    auto next = seed;
    next.InteractionTimeSeconds += 0.125;
    ++next.InteractionRevision;
    next.Wind[0].TimeSeconds -= 0.125f;
    Require(!selection.TryAdopt(version(2), { 241, next, capture + std::chrono::milliseconds(20) }),
        "Latest adopted a rewound wind field");
    next.Wind[0].TimeSeconds = seed.Wind[0].TimeSeconds + 0.125f;
    next.Wind[1].TimeSeconds = seed.Wind[1].TimeSeconds + 0.125f;
    Require(selection.TryAdopt(version(2), { 241, next, capture + std::chrono::milliseconds(20) }), "Latest rejected the first continuous imported field");
    Require(!selection.TryAdopt(version(2), { 241, next, capture + std::chrono::milliseconds(20) }) && selection.Displayed().Sequence == 241 &&
        selection.Displayed().Layout.InteractionTimeSeconds == 4.125,
        "Latest repeat advanced or replaced the last displayed state");
    const auto display = capture + std::chrono::milliseconds(100);
    Require(selection.AgeMs(display) == 80 && selection.OverAge(display, 75) &&
        !selection.OverAge(display, 0) && selection.Displayed().Sequence == 241,
        "Latest age changed its compatible selection or ignored the diagnostic switch");
    auto incompatible = next;
    ++incompatible.InteractionResetRevision;
    Require(!selection.TryAdopt(version(3), { 242, incompatible, capture + std::chrono::milliseconds(40) }) && selection.Displayed().Sequence == 241,
        "Latest display crossed a reset within one epoch");
    auto previousEpoch = version(3);
    previousEpoch.VersionEpoch = static_cast<Epoch>(1);
    Require(!selection.TryAdopt(previousEpoch, { 242, next, capture + std::chrono::milliseconds(40) }) && selection.Displayed().Sequence == 241,
        "Latest display accepted a late import from the previous route epoch");

    // Presence may change while paused without advancing either simulation clock.
    ++next.ActorRevision;
    Require(selection.TryAdopt(version(4), { 242, next, capture + std::chrono::milliseconds(40) }) &&
        selection.Displayed().Layout.InteractionTimeSeconds == 4.125,
        "Latest did not adopt a compatible paused field update");
}

static void CheckPackedGrassFields(GDX12Device& device, GDX12DeviceResources& resources,
    const RenderPipelineCommonData& common)
{
    using Producer = GDX12GrassFieldProducer;
    using InputQueue = GDX12GrassFieldInputQueue;
    InputQueue backlog;
    InputQueue::Tick tick;
    tick.InteractionArea = { 0, 0, 100, 100 };
    tick.DeltaSeconds = 0.125f;
    tick.Actors = { {{0, 5, 0, 55}, {-10, 5, 0, 190}} };
    GDX12WindInstance analytic;
    analytic.Id = 10;
    analytic.Implementation = GDX12WindImplementation::Analytic;
    analytic.GridResolution = 8;
    analytic.DeltaSeconds = 0.125f;
    auto projected = analytic;
    projected.Id = 20;
    projected.Implementation = GDX12WindImplementation::ProjectedGrid;
    projected.PressureIterations = 3;
    tick.Wind = { projected, analytic };

    for (uint64_t sequence = 1; sequence <= InputQueue::Capacity; ++sequence)
    {
        tick.Sequence = sequence;
        Require(backlog.Push(tick), "Grass input backlog rejected a free credit");
    }
    tick.Sequence++;
    Require(!backlog.Push(tick) && backlog.FirstMissing() == tick.Sequence,
        "Grass backlog failed to latch its first missing input");
    Require(backlog.PendingSeconds() == InputQueue::Capacity * 0.125 && backlog.MissingSeconds() == 0.125,
        "Grass backlog input time accounting differs");
    backlog.Commit(1);
    tick.Sequence++;
    Require(!backlog.Push(tick) && backlog.FirstMissing() == tick.Sequence - 1 && backlog.Front()->Sequence == 2,
        "Grass backlog crossed a missing actor segment");
    backlog.Reset();
    Require(backlog.SupersededSeconds() == (InputQueue::Capacity - 1) * 0.125 &&
        !backlog.Front() && !backlog.FirstMissing(), "Grass epoch reset did not retire pending input");

    std::vector<XMFLOAT4> baseline;
    for (bool async : {false, true})
    {
        auto* queue = async ? device.TryGetComputeQueue() : device.GetCommandQueue();
        Require(queue != nullptr, "Grass producer validation requires a COMPUTE queue");
        Producer producer(device, *resources.SRV_UAV_Heap, 1);
        producer.Prepare(0, tick.Inputs());
        const auto staged = producer.Snapshot(0);
        producer.Cancel(0);
        auto source = tick;
        producer.Prepare(0, source.Inputs());
        source.Actors.clear();
        source.Wind.clear();
        auto layout = producer.Snapshot(0);
        Require(layout.IsCompatibleWith(staged) && layout.InteractionRevision == staged.InteractionRevision &&
            layout.InteractionTimeSeconds == 0.125 && layout.Wind[0].TimeSeconds == staged.Wind[0].TimeSeconds &&
            layout.Wind[0].WindDebtSeconds > 0, "No-submit retry advanced the grass producer clock");
        constexpr UINT cells = Producer::InteractionResolution * Producer::InteractionResolution;
        Require(layout.Wind.size() == 2 && layout.Presence.FirstElement == 0 && layout.Trail.FirstElement == cells &&
            layout.Wind[0].Owner == analytic.Id && layout.Wind[1].Owner == projected.Id &&
            layout.Wind[0].Velocity.FirstElement == 2 * cells &&
            layout.Wind[1].Velocity.FirstElement == 2 * cells + 64 &&
            layout.ElementCount == 2 * cells + 128, "Packed grass regions are incomplete or unsorted");

        if (!async) CheckGrassLatestHandoff(layout);

        GDX12Resource packed;
        packed.D3DResource = device.CreateResource(D3D12_HEAP_TYPE_DEFAULT,
            CD3DX12_RESOURCE_DESC::Buffer(UINT64(layout.ElementCount) * sizeof(XMFLOAT4)),
            D3D12_RESOURCE_STATE_COMMON);
        auto* commands = queue->GetCommandList();
        commands->ResourceBarrier({ packed.GetCopyDestBarrier() });
        producer.Record(*commands, 0, packed.D3DResource.Get());
        commands->ResourceBarrier({ packed.GetTransitionBarrier(D3D12_RESOURCE_STATE_COMMON) });
        queue->ExecuteCommandList(commands);
        producer.Commit(0);
        queue->Flush();

        const auto payload = ReadInteractionBuffer<XMFLOAT4>(device, packed, layout.ElementCount);
        const auto checkRegion = [&](Producer::FieldBuffer& buffer, const Producer::Region& region)
        {
            const auto values = ReadInteractionBuffer<XMFLOAT4>(device, buffer.GetResource(), region.ElementCount);
            Require(std::memcmp(payload.data() + region.FirstElement, values.data(), values.size() * sizeof(XMFLOAT4)) == 0,
                "Packed grass snapshot differs from the producer field");
        };
        checkRegion(producer.Presence(), layout.Presence);
        checkRegion(producer.Trail(), layout.Trail);
        for (const auto& field : layout.Wind) checkRegion(*producer.FindWind(field.Owner)->Current, field.Velocity);
        Require(std::any_of(payload.begin(), payload.begin() + cells, [](const auto& cell) { return cell.z > 0; }),
            "Actor was missing from the packed grass field");
        if (async)
            Require(std::memcmp(payload.data(), baseline.data(), payload.size() * sizeof(XMFLOAT4)) == 0,
                "Packed grass fields differ between DIRECT and COMPUTE");
        else baseline = payload;

        auto snapshot = std::make_shared<const GDX12GrassFieldSnapshot>(*resources.SRV_UAV_Heap, packed, layout);
        GDX12GrassRenderer renderer(&resources);
        GDX12GrassInstance grass;
        grass.Id = 1;
        grass.RootsCm = {{0, 0, 0}};
        grass.WindId = analytic.Id;
        grass.HeightVariation = 0;
        grass.Segments = 2;
        auto controls = GetGrassWindControls();
        controls.GrassEnabled = controls.WindEnabled = true;
        controls.GrassQuality = controls.WindQuality = 2;
        const GDX12GrassRenderer::DisplayedFields selected{ snapshot, snapshot };
        renderer.BeginFrame(common, {grass}, tick.Wind, controls, {}, 0, &selected);
        const auto presence = renderer.InteractionField();
        Require(presence.Resource == &packed && presence.SRV.ptr == snapshot->Presence().SRV.ptr &&
            presence.Resolution == layout.InteractionGrid && presence.Area.z == 100,
            "Selected grass field and Firefly binding disagree");
        const Vector3 eye(0, 70, -180);
        const Matrix vp = XMMatrixLookAtLH(eye, Vector3(0, 25, 0), Vector3::UnitY) *
            XMMatrixPerspectiveFovLH(XMConvertToRadians(60), 1, 1, 10000);
        auto view = GrassTestView(resources, GDX12GrassRenderer::Stage::Shadow, vp, eye,
            resources.FrameConstants[resources.CurrFrameConstantsIndex]->CameraCB->GetElementAddress(0));
        renderer.PrepareView(view, 0);
        const auto accesses = renderer.ExpansionAccesses(view, 0);
        Require(std::count_if(accesses.begin(), accesses.end(), [&](const auto& access) { return access.Resource == &packed; }) == 4,
            "Grass expansion did not declare the selected wind/trail reads");
        auto* direct = device.GetCommandQueue();
        commands = direct->GetCommandList();
        renderer.PrepareGPU(commands);
        commands->ResourceBarrier({ packed.GetNonPixelShaderResourceBarrier() });
        Require(renderer.Expand(commands, view, 0, true), "Selected fields could not expand shadow grass");
        direct->ExecuteCommandList(commands);
        direct->Flush();

        struct Vertex { XMFLOAT3 Position, Previous, Normal; XMFLOAT2 UV; };
        const auto vertices = ReadInteractionBuffer<Vertex>(device, *renderer.DrawAccesses().front().Resource, 6);
        const float tipHeight = (vertices[4].Position.y + vertices[5].Position.y) * 0.5f;
        Require(tipHeight > 5 && tipHeight < 35 &&
            std::memcmp(&vertices[4].Position, &vertices[4].Previous, sizeof(XMFLOAT3)) == 0,
            "Packed trail did not bend the shadow geometry or reset its motion history");

        auto changed = tick;
        changed.InteractionArea.x = 500;
        changed.InteractionResetRevision++;
        changed.Wind[0].CenterCm.x = 200;
        producer.Prepare(0, changed.Inputs());
        Require(!layout.IsCompatibleWith(producer.Snapshot(0)) && producer.Snapshot(0).InteractionTimeSeconds == changed.DeltaSeconds,
            "A moved grass field reused a prior display layout or clock");
        producer.Cancel(0);
        producer.Prepare(0, tick.Inputs());
        Require(layout.IsCompatibleWith(producer.Snapshot(0)) && producer.Snapshot(0).InteractionTimeSeconds == 0.25,
            "An unsubmitted field reset changed the committed producer state");
        producer.Cancel(0);

        // The receiving solver imports actual velocity/trail and continues at the donor clock.
        Producer receiving(device, *resources.SRV_UAV_Heap, 1);
        receiving.PrepareSeed(0, tick.Inputs(), layout);
        commands = queue->GetCommandList();
        commands->ResourceBarrier({ packed.GetCopySourceBarrier() });
        receiving.RecordSeed(*commands, 0, *packed.D3DResource.Get());
        commands->ResourceBarrier({ packed.GetTransitionBarrier(D3D12_RESOURCE_STATE_COMMON) });
        queue->ExecuteCommandList(commands);
        receiving.Commit(0);
        queue->Flush();
        Require(receiving.Snapshot(0).InteractionTimeSeconds == layout.InteractionTimeSeconds &&
            receiving.Snapshot(0).Wind[1].WindDebtSeconds == layout.Wind[1].WindDebtSeconds,
            "Grass handoff did not restore the authoritative solver clock and debt");

        const auto advance = [&](Producer& solver, const Producer::Inputs* inputs, size_t count)
        {
            solver.PrepareBatch(0, inputs, count);
            auto* list = queue->GetCommandList();
            list->ResourceBarrier({ packed.GetCopyDestBarrier() });
            solver.Record(*list, 0, packed.D3DResource.Get());
            list->ResourceBarrier({ packed.GetTransitionBarrier(D3D12_RESOURCE_STATE_COMMON) });
            queue->ExecuteCommandList(list);
            solver.Commit(0);
            queue->Flush();
            return ReadInteractionBuffer<XMFLOAT4>(device, packed, layout.ElementCount);
        };
        auto paused = tick;
        paused.DeltaSeconds = 0;
        paused.Actors.front().PositionRadius.x += 30;
        for (auto& wind : paused.Wind) wind.DeltaSeconds = 0;
        const auto pausedInput = paused.Inputs();
        const auto pausedFields = advance(producer, &pausedInput, 1);
        const auto pausedReplica = advance(receiving, &pausedInput, 1);
        Require(std::memcmp(pausedFields.data(), pausedReplica.data(), pausedFields.size() * sizeof(XMFLOAT4)) == 0 &&
            std::memcmp(pausedFields.data(), payload.data(), cells * sizeof(XMFLOAT4)) != 0,
            "Paused grass failed to refresh actor presence in both solvers");
        for (size_t i = 0; i < layout.Wind.size(); ++i)
        {
            const auto& before = layout.Wind[i];
            const auto& after = producer.Snapshot(0).Wind[i];
            const auto& seeded = receiving.Snapshot(0).Wind[i];
            const auto offset = before.Velocity.FirstElement;
            Require(before.WindDebtSeconds > 0 && after.WindDebtSeconds == before.WindDebtSeconds &&
                seeded.WindDebtSeconds == before.WindDebtSeconds && after.TimeSeconds == before.TimeSeconds &&
                seeded.TimeSeconds == before.TimeSeconds &&
                std::memcmp(pausedFields.data() + offset, payload.data() + offset,
                    before.Velocity.ElementCount * sizeof(XMFLOAT4)) == 0,
                "Paused wind advanced its field, clock or debt");
        }

        auto next = paused;
        next.Actors.front().PreviousHeight = next.Actors.front().PositionRadius;
        next.Actors.front().PreviousHeight.w = 190;
        next.Actors.front().PositionRadius.x += 40;
        next.DeltaSeconds = 0.015625f;
        for (auto& wind : next.Wind) wind.DeltaSeconds = next.DeltaSeconds;
        auto following = next;
        following.Actors.front().PreviousHeight.x = next.Actors.front().PositionRadius.x;
        following.Actors.front().PositionRadius.x += 20;
        const std::array<Producer::Inputs, 2> steps{ next.Inputs(), following.Inputs() };
        advance(producer, &steps[0], 1);
        const auto uninterrupted = advance(producer, &steps[1], 1);
        const auto transferred = advance(receiving, steps.data(), steps.size());
        Require(producer.Snapshot(0).InteractionTimeSeconds == receiving.Snapshot(0).InteractionTimeSeconds &&
            producer.Snapshot(0).Wind[0].TimeSeconds == receiving.Snapshot(0).Wind[0].TimeSeconds &&
            producer.Snapshot(0).Wind[1].TimeSeconds == receiving.Snapshot(0).Wind[1].TimeSeconds &&
            std::memcmp(uninterrupted.data(), transferred.data(), uninterrupted.size() * sizeof(XMFLOAT4)) == 0,
            "Grass solver migration or cadence batching changed the ordered logical steps");
        for (size_t i = 0; i < layout.Wind.size(); ++i)
        {
            const auto& before = layout.Wind[i];
            const auto& resumed = producer.Snapshot(0).Wind[i];
            const auto offset = before.Velocity.FirstElement;
            Require(resumed.WindDebtSeconds < before.WindDebtSeconds && resumed.TimeSeconds > before.TimeSeconds &&
                resumed.WindDebtSeconds == receiving.Snapshot(0).Wind[i].WindDebtSeconds &&
                std::memcmp(uninterrupted.data() + offset, pausedFields.data() + offset,
                    before.Velocity.ElementCount * sizeof(XMFLOAT4)) != 0,
                "Resumed wind failed to advance the field and drain accumulated debt");
        }
    }
}

static void CheckGrassInteraction(GDX12Device& device, GDX12DeviceResources& resources,
    const RenderPipelineCommonData& common, GDX12Texture& depth)
{
    CheckPackedGrassFields(device, resources, common);
    struct Vertex { XMFLOAT3 Position, Previous, Normal; XMFLOAT2 UV; };
    struct Particle { XMFLOAT3 Position; float Age; XMFLOAT3 Previous; float Lifetime;
        XMFLOAT3 Velocity; float Rotation; XMFLOAT4 Rest; };
    static_assert(sizeof(Vertex) == 44 && sizeof(Particle) == 64);
    auto* queue = device.GetCommandQueue();
    auto frozen = common; frozen.ResetHistory = false;
    const UINT savedSlot = resources.CurrFrameConstantsIndex;
    const auto savedEmitters = resources.ParticleEmitters;
    auto savedSimulation = std::move(resources.ParticleSimulation);
    std::filesystem::create_directories("Build/GrassInteraction");
    std::ofstream report("Build/GrassInteraction/numerical.csv");
    report << "wind,async,frame,tip_y,previous_tip_y,root_y,particle_y,vx,vy,vz,max_presence\n";
    const auto center = BistroArch::GrassPosition;
    const auto eye = center + Vector3(0, 70, -180);
    const Matrix vp = XMMatrixLookAtLH(eye, center + Vector3(0, 25, 0), Vector3::UnitY) *
        XMMatrixPerspectiveFovLH(XMConvertToRadians(60), 1, 1, 10000);
    for (int windMode : {0, 1, 2})
    {
        std::vector<Vertex> baseline;
        std::vector<Particle> baselineParticles;
        for (bool async : {false, true})
        {
            GDX12GrassRenderer renderer(&resources);
            resources.ParticleSimulation = std::make_unique<GDX12ParticleSimulation>();
            GDX12GrassInstance grass;
            grass.Id = 9901; grass.RootsCm = {{0,0,0}}; grass.World = Matrix::CreateTranslation(center);
            grass.HeightCm = 40; grass.HeightVariation = 0; grass.Segments = 2; grass.WidthCm = 2;
            GDX12WindInstance wind; wind.Id = 9902; wind.CenterCm = Vector2(center.x, center.z);
            wind.Implementation = windMode == 2 ? GDX12WindImplementation::ProjectedGrid : GDX12WindImplementation::Analytic;
            wind.GridResolution = 16; wind.PressureIterations = 4; wind.DeltaSeconds = 1.0f / 30;
            grass.WindId = windMode ? wind.Id : 0;
            auto controls = GetGrassWindControls(); controls.GrassEnabled = true; controls.GrassQuality = 2;
            controls.WindEnabled = windMode != 0; controls.WindQuality = 2;
            InteractionPreparation prepare(renderer);
            GrassTestViews views(depth);
            GrassTestExpansion expand(renderer, views, 0);
            GDX12ParticleSimulationPass particles;
            particles.Setup(&resources, nullptr, &frozen); particles.SetInputs({&depth});
            GDX12RenderGraph graph(&device);
            graph.ImportShared(GDX12RenderGraph::SharedResource::Scene, "Test scene");
            graph.ImportLink("Test depth", &depth);
            const auto preparation = graph.AddPass(prepare);
            const auto expansion = graph.AddPass(expand);
            graph.DependsOn(expansion, preparation);
            graph.AddPass(particles); graph.Compile(false);
            GDX12ParticleEmitter emitter;
            emitter.Id = 9910; emitter.MaxParticles = 1; emitter.BurstTotal = 1; emitter.EmissionRate = 0;
            emitter.World = Matrix::CreateTranslation(center + Vector3(0,100,0));
            emitter.LifetimeMin = emitter.LifetimeMax = 20; emitter.SpeedMin = emitter.SpeedMax = 0;
            emitter.Acceleration = Vector3::Zero; emitter.Wind = Vector3::Zero; emitter.Drag = 2;
            emitter.VolumeEnabled = true; emitter.VolumeHalfExtents = Vector3(120,100,120);
            emitter.Geometry = ParticleGeometry::Firefly; emitter.InteractionEnabled = true;
            Set(L"Render.Particles.AsyncCompute", async ? "true" : "false");
            float compressedHeight = 0, lastHeight = 0, compressedVelocity = 0;
            for (UINT frame = 0; frame < 10; ++frame)
            {
                resources.CurrFrameConstantsIndex = frame % static_cast<UINT>(resources.FrameConstants.size());
                std::vector<BistroInteractionSource> sources;
                if (frame == 1 || frame == 2)
                    for (float offset : {-20.0f, 20.0f})
                        sources.push_back({{center.x + offset, center.y, center.z, 65},
                            {center.x + offset, center.y, center.z, 190}});
                // Dense overlapping sources must not multiply the escape speed.
                if (frame == 7)
                    for (UINT i = 0; i < 32; ++i)
                        sources.push_back({{center.x - 20, center.y, center.z, 65}, {center.x - 20, center.y, center.z, 190}});
                if (frame == 9)
                    sources.push_back({{center.x + 2000, center.y, center.z, 65}, {center.x, center.y, center.z, 190}});
                frozen.ResetHistory = frame == 3; // Camera history reset must not erase simulation.
                const float dt = frame == 2 ? 0 : frame >= 3 && frame <= 6 ? 0.5f : 1.0f / 30;
                wind.DeltaSeconds = dt;
                wind.SpeedCmPerSecond = frame == 7 ? 500.0f : 0.0f;
                wind.GustAmplitude = 0; wind.ForcePerSecond = 100;
                renderer.BeginFrame(frozen, {grass}, windMode ? std::vector<GDX12WindInstance>{wind} : std::vector<GDX12WindInstance>{}, controls, sources, dt);
                views.Views[0] = GrassTestView(resources, GDX12GrassRenderer::Stage::Depth, vp, eye,
                    resources.FrameConstants[resources.CurrFrameConstantsIndex]->CameraCB->GetElementAddress(0));
                // Simulation must recover while the root is offscreen.
                if (frame == 4 || frame == 5) views.Views[0].ViewProjection = Matrix::CreateTranslation(100000,0,0) * vp;
                renderer.PrepareView(views.Views[0], 0);
                emitter.DeltaTime = (std::min)(dt, 0.1f);

                resources.ParticleEmitters = {emitter};
                resources.ParticleSimulation->BeginFrame(&resources, frozen);
                resources.ParticleSimulation->SetInteractionField(renderer.InteractionField());
                auto* commands = queue->GetCommandList();
                resources.ParticleSimulation->InitializePools(commands, &resources);
                commands = graph.Execute(commands, async ? device.TryGetComputeQueue() : nullptr);
                queue->ExecuteCommandList(commands); queue->Flush();
                Require(graph.GetStatistics().AsyncPasses == (async ? 2u : 0u), "Interaction DIRECT/COMPUTE selection failed");
                const auto presence = ReadInteractionBuffer<XMFLOAT4>(device, *renderer.InteractionField().Resource, 64 * 64);
                float maximumPresence = 0;
                for (const auto& cell : presence) maximumPresence = (std::max)(maximumPresence, cell.z);
                if (sources.empty()) Require(maximumPresence == 0, "Particle field retained an empty grass trail");
                const auto particle = ReadInteractionBuffer<Particle>(device, *resources.ParticleSimulation->GetPoolResources().at(0), 1).at(0);
                Require(std::isfinite(particle.Velocity.y) && std::abs(particle.Position.x - center.x) <= 120.01f &&
                    std::abs(particle.Position.z - center.z) <= 120.01f && particle.Position.y <= center.y + 200.01f,
                    "Interaction escaped the swarm volume");
                Require(std::hypot(particle.Velocity.x, particle.Velocity.z) <= 180.01f && particle.Velocity.y <= 220.01f,
                    "Overlapping sources multiplied the particle escape speed");
                if (frame == 1) { Require(particle.Velocity.y > 20, "Firefly failed to escape upward"); compressedVelocity = particle.Velocity.y; }
                if (frame == 2) Require(particle.Velocity.y == compressedVelocity, "Paused particle advanced");
                if (frame == 6) Require(particle.Velocity.y < compressedVelocity, "Firefly failed to return to normal drag behavior");
                if (async)
                    Require(std::memcmp(&particle, &baselineParticles.at(frame), sizeof(Particle)) == 0, "DIRECT/async particle state differs");
                else baselineParticles.push_back(particle);
                if (frame == 4 || frame == 5) continue;
                const auto vertices = ReadInteractionBuffer<Vertex>(device, *renderer.DrawAccesses().at(0).Resource, 6);
                const float tipY = (vertices[4].Position.y + vertices[5].Position.y) * 0.5f - center.y;
                const float previousY = (vertices[4].Previous.y + vertices[5].Previous.y) * 0.5f - center.y;
                Require(std::abs((vertices[0].Position.y + vertices[1].Position.y) * 0.5f - center.y) < 0.001f,
                    "Grass interaction moved the fixed root");
                const Vector3 tip((vertices[4].Position.x + vertices[5].Position.x) * 0.5f - center.x, tipY,
                    (vertices[4].Position.z + vertices[5].Position.z) * 0.5f - center.z);
                const Vector3 previousTip((vertices[4].Previous.x + vertices[5].Previous.x) * 0.5f - center.x, previousY,
                    (vertices[4].Previous.z + vertices[5].Previous.z) * 0.5f - center.z);
                Require(tip.Length() <= 40.001f && previousTip.Length() <= 40.001f,
                    "Combined wind and contact stretched the blade beyond its length");
                if (frame == 0) Require(std::abs(tipY - 40) < 0.01f, "Undisturbed blade height changed");
                if (frame == 1) { Require(tipY < 20 && tipY > 7, "Opposing sources did not depress the blade"); compressedHeight = tipY;
                    Require(std::abs(previousY - 40) < 0.01f, "Grass motion history lost the previous shape"); }
                if (frame == 2) Require(tipY == compressedHeight, "dt=0 advanced the grass trail");
                if (frame == 3) Require(tipY > 30 && tipY < 35, "Camera reset erased the recovering trail");
                if (frame == 9) Require(tipY > lastHeight && maximumPresence == 0, "Teleport stamped a false path");
                Require(std::abs((vertices[0].Position.x + vertices[1].Position.x) * 0.5f - center.x) < 0.001f &&
                    std::abs((vertices[0].Position.z + vertices[1].Position.z) * 0.5f - center.z) < 0.001f, "Grass root moved sideways");
                if (frame == 6) Require(tipY > 39.8f, "Grass failed to recover after two offscreen seconds");
                if (frame == 8) Require(tipY > lastHeight, "Grass recovery did not resume after a dense stamp");
                if (async)
                    for (UINT i = 0; i < vertices.size(); ++i)
                        Require(std::memcmp(&vertices[i], &baseline.at(frame * 6 + i), sizeof(Vertex)) == 0, "DIRECT/async grass vertices differ");
                else
                {
                    if (baseline.size() < (frame + 1) * 6) baseline.resize((frame + 1) * 6);
                    std::copy(vertices.begin(), vertices.end(), baseline.begin() + frame * 6);
                }
                report << windMode << ',' << async << ',' << frame << ',' << tipY << ',' << previousY << ',' << center.y
                    << ',' << particle.Position.y << ',' << particle.Velocity.x << ',' << particle.Velocity.y << ',' << particle.Velocity.z << ',' << maximumPresence << '\n';
                lastHeight = tipY;
            }
            resources.ParticleSimulation.reset();
        }
    }
    resources.ParticleSimulation = std::move(savedSimulation);
    resources.ParticleEmitters = savedEmitters;
    resources.CurrFrameConstantsIndex = savedSlot;
    Set(L"Render.Particles.AsyncCompute", "false");
    std::cout << "Grass interaction: depression, stationary pressure, opposing/dense sources, dt0, offscreen recovery, Firefly response, bounds and exact DIRECT/async GPU state parity passed.\n";
}

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
    commands->ResourceBarrier({ source.GetCopySourceBarrier() });
    commands->GetCommandList()->CopyBufferRegion(target.Get(), 0, source.D3DResource.Get(), 0, UINT64(count) * sizeof(T));
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

static void CheckGrassInteraction(GDX12Device& device, GDX12DeviceResources& resources,
    const RenderPipelineCommonData& common, GDX12Texture& depth)
{
    struct Vertex { XMFLOAT3 Position, Previous, Normal; XMFLOAT2 UV; };
    struct Particle { XMFLOAT3 Position; float Age; XMFLOAT3 Previous; float Lifetime;
        XMFLOAT3 Velocity; float Rotation; XMFLOAT4 Rest; XMUINT2 Identity; };
    static_assert(sizeof(Vertex) == 44 && sizeof(Particle) == 72);
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
            particles.Setup(&resources, &frozen); particles.SetInputs({&depth});
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

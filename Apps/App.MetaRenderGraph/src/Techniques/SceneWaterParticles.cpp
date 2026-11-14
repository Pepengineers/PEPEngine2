#include "SceneWaterParticles.h"
#include "Engine.Core/BenchmarkEngine.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12TextureResource.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"

namespace Meta
{
namespace
{
struct SimulationBinding
{
    std::unique_ptr<GDX12ParticleSimulation>& Primary;
    std::unique_ptr<GDX12ParticleSimulation>& Local;
    SimulationBinding(std::unique_ptr<GDX12ParticleSimulation>& primary,
        std::unique_ptr<GDX12ParticleSimulation>& local) : Primary(primary), Local(local) { std::swap(Primary, Local); }
    ~SimulationBinding() { std::swap(Primary, Local); }
};
}

struct SceneWaterParticles::Path
{
    GDX12FluidSurfacePass Water;
    std::unique_ptr<GDX12ParticleSimulation> Simulation = std::make_unique<GDX12ParticleSimulation>();
    GDX12ParticlesPass Particles;
    GDX12WBOITTransparencyPass Far{GDX12GeometryDepthPartition::Far};
    GDX12WBOITTransparencyPass Near{GDX12GeometryDepthPartition::Near};
    GDX12WBOITCompositionPass FarComposition, NearComposition;
    bool Initialized = false;
    UINT Width = 0, Height = 0;
    Path() { Far.SetUnbatchedDraws(true); Near.SetUnbatchedDraws(true); Water.SetLocalExecution(true); Particles.SetLocalExecution(true); NearComposition.SetApplyOpaqueAerialPerspective(false); }
    ~Path()
    {
        Water.ClearDenendencies(); Particles.ClearDenendencies();
        Far.ClearDenendencies(); Near.ClearDenendencies();
        FarComposition.ClearDenendencies(); NearComposition.ClearDenendencies();
    }
};
SceneWaterParticles::SceneWaterParticles(RenderPipelineContext& context)
    : _resources(context.Primary), _common(context.Common),
      _scenes(BenchmarkEngine::GetLocator().GetModule<SceneManagerModule>()),
      _collector(std::make_unique<BistroWaterRendering>(_scenes))
{
    for (auto& path : _paths) path = std::make_unique<Path>();
}
SceneWaterParticles::~SceneWaterParticles() = default;
void SceneWaterParticles::Prepare(float delta)
{
    _instances = _collector->CollectScene(delta);
    _groups = {};
    _groups[1].DepthCollision = true;
    for (const auto& emitter : _resources.ParticleEmitters)
        _groups[emitter.DepthCollision ? 1 : 0].Emitters.push_back(emitter.Id);
}
void SceneWaterParticles::Reset() { _reset = true; }
void SceneWaterParticles::Resize()
{
    for (auto& path : _paths) path = std::make_unique<Path>();
    _depth = _motion = _particleMotion = nullptr;
    _resources.ParticleMotion = nullptr;
}
GDX12Texture* SceneWaterParticles::Append(Graph& graph, std::vector<ID3D12Resource*>& bindings,
    SceneGeometry& geometry, SceneEnvironment& environment, GDX12Texture* reflectedHDR,
    GDX12Texture* probes, bool serial)
{
    if (_reset) { _resources.Device->GetCommandQueue()->Flush(); Resize(); _reset = false; }
    auto& path = *_paths[serial ? 0 : 1];
    auto& scene = geometry._slots.at(_resources.CurrFrameConstantsIndex);
    auto& frame = *_resources.FrameConstants[_resources.CurrFrameConstantsIndex];
    const UINT slot = _resources.CurrFrameConstantsIndex;
    _work.Commands.clear();
    auto& water = path.Water;
    water.Setup(&_resources, &_common);
    water.SetInputs({scene.DepthView.get(), scene.Color[1].get()});
    water.Initialize();
    water.SetInstances(_instances);
    water.PrepareFrame();
    _depth = water.GetOutput(0)->GetTexture();
    _motion = water.GetOutput(1)->GetTexture();
    if (path.Initialized && (path.Width != _depth->GetWidth() || path.Height != _depth->GetHeight()))
        throw std::logic_error("U6 size change requires drained Resize");
    path.Width = _depth->GetWidth(); path.Height = _depth->GetHeight();
    // Each diagnostic path retains its own pools, head, burst consumption and
    // previous displayed water transform. No production offload worker exists.
    SimulationBinding preparedSimulation(_resources.ParticleSimulation, path.Simulation);
    auto* simulation = _resources.ParticleSimulation.get();
    simulation->SetInteractionField(geometry.InteractionField());
    simulation->BeginFrame(&_resources, _common, true);
    auto& particles = path.Particles;
    particles.Setup(&_resources, &_common);
    const auto inputs = [&](GDX12WBOITTransparencyPass& pass) {
        pass.Setup(&_resources, &_common);
        pass.SetInputs({scene.DepthView.get(), geometry._shadows->LightingSun(slot), environment.Output(0),
            environment.Output(1), environment.Output(2), geometry._shadows->LightingLocal(slot), _depth});
    };
    inputs(path.Far); inputs(path.Near);
    if (!path.Initialized) { path.Far.Initialize(); path.Near.Initialize(); }
    particles.SetInputs({_depth, path.Near.GetOutput(0), path.Near.GetOutput(1)});
    particles.Initialize();
    particles.BeginFrame();
    _particleMotion = particles.MotionOutput();
    path.FarComposition.Setup(&_resources, &_common);
    path.NearComposition.Setup(&_resources, &_common);
    path.FarComposition.SetInputs({reflectedHDR, path.Far.GetOutput(0), path.Far.GetOutput(1), scene.DepthView.get()});
    auto* waterHDR = water.NativeColor();
    if (!waterHDR) waterHDR = path.FarComposition.GetOutput(0)->GetTexture();
    path.NearComposition.SetInputs({waterHDR, path.Near.GetOutput(0), path.Near.GetOutput(1), _depth});
    if (!path.Initialized) { path.FarComposition.Initialize(); path.NearComposition.Initialize(); path.Initialized = true; }
    path.FarComposition.RefreshInputLinks(); path.NearComposition.RefreshInputLinks();
    // Physical read declarations include all global texture descriptors used by
    // water optics, transparency and aerial composition (not just their links).
    auto global = environment.Reads(graph, bindings);
    const auto resource = [&](ID3D12Resource* native) { return NativeWork::ResourceFor(graph, bindings, native, "U6 resource"); };
    const auto readState = [](ID3D12Resource* native) {
        if (native->GetDesc().Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) return D3D12_RESOURCE_STATE_COMMON;
        return native->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? D3D12_RESOURCE_STATE_GENERIC_READ :
            D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    };
    auto read = [&](GDX12Resource* value) { auto* native = value->D3DResource.Get(); return Access{resource(native), Mode::Read, State(readState(native))}; };
    const auto textureRead = [&](GDX12Texture* texture) { return read(texture->GetResource()); };
    global.push_back(textureRead(probes));
    global.push_back(read(&frame.ReflectionProbes->GetResource()));
    global.push_back(read(&frame.CameraCB->GetResource()));
    global.push_back(read(&frame.SkyCB->GetResource()));
    global.push_back(read(&frame.ShadowCB->GetResource()));
    global.push_back(read(&frame.ShadowLights->GetResource()));
    Id tail = Invalid;
    const auto packet = [&](const std::string& name, std::vector<Access> accesses,
                            std::function<void(GDX12CommandList*)> record) {
        std::vector<Access> unique;
        for (auto a : accesses)
        {
            // All packet boundaries are explicit read states. Private transitions
            // return to that state; DEFAULT-buffer decay is executor-owned.
            a.Required = readState(bindings.at(a.Resource));
            auto found = std::find_if(unique.begin(), unique.end(), [&](auto b) { return a.Resource == b.Resource; });
            if (found == unique.end()) unique.push_back(a);
            else if (a.Usage != Mode::Read) found->Usage = Mode::ReadWrite;
        }
        std::vector<GDX12RecordingStates::Binding> states;
        for (auto a : unique) states.emplace_back(bindings.at(a.Resource), D3D12_RESOURCE_STATES(a.Required));
        std::vector<Id> dependencies; if (tail != Invalid) dependencies.push_back(tail);
        tail = _work.Add(graph, name, unique, [this, &path, states, record](auto* commands) {
            GDX12RecordingStates tracker(states);
            GDX12CommandList borrowed(commands);
            // Original render helpers look up the active primary simulation.
            // Install only this diagnostic path during its owner callback.
            SimulationBinding active(_resources.ParticleSimulation, path.Simulation);
            try { record(&borrowed); }
            catch (...) { _reset = true; throw; }
            if (borrowed.HasSubmissionSignals()) throw std::logic_error("U6 cannot publish private submission signals");
            tracker.Finish(commands);
        }, dependencies);
        graph.Operations[tail].OwnerRecording = true;
    };
    const auto appendResources = [&](std::vector<Access>& accesses, const std::vector<GDX12Resource*>& resources, Mode mode) {
        for (auto* value : resources)
        {
            auto a = read(value);
            D3D12_HEAP_PROPERTIES heap{};
            ThrowIfFailed(value->D3DResource->GetHeapProperties(&heap, nullptr));
            a.Usage = heap.Type == D3D12_HEAP_TYPE_UPLOAD ? Mode::Read : mode;
            accesses.push_back(a);
        }
    };
    auto waterAccess = global;
    appendResources(waterAccess, water.NativeResources(), Mode::ReadWrite);
    waterAccess.push_back(textureRead(scene.DepthView.get())); waterAccess.push_back(textureRead(scene.Color[1].get()));
    packet("water seed/SPH hash-sort-density-integrate", waterAccess, [&water](auto* commands) { water.ExecuteSimulation(commands); });
    packet("water surface/thickness/NRF/merge depth-motion", waterAccess, [&water](auto* commands) { water.Execute(commands); });
    auto simAccess = global;
    appendResources(simAccess, simulation->GetPoolResources(), Mode::ReadWrite);
    appendResources(simAccess, simulation->GetConstantResources(slot), Mode::Read);
    if (auto* interaction = simulation->GetInteractionResource()) simAccess.push_back(read(interaction));
    simAccess.push_back(textureRead(scene.DepthView.get()));
    packet("particle pool initialization/reset", simAccess, [this, simulation](auto* commands) { simulation->InitializePools(commands, &_resources); });
    auto* opaqueDepth = scene.DepthView.get();
    packet("particle simulate/death/spawn/burst/publish", simAccess, [this, simulation, opaqueDepth](auto* commands) {
        simulation->Execute(commands, &_resources, _common, opaqueDepth);
    });
    auto particleAccess = simAccess;
    appendResources(particleAccess, particles.NativeResources(), Mode::ReadWrite);
    particleAccess.push_back(textureRead(_depth));
    if (_particleMotion) // Active particle rendering validated its atlas inputs in BeginFrame.
        for (const auto& emitter : _resources.ParticleEmitters)
            if (emitter.Texture)
            {
                auto* atlas = emitter.Texture->PrimaryDeviceTexture;
                if (!atlas) throw std::logic_error("Particle atlas has no primary texture");
                constexpr State published = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
                const Id id = NativeWork::ImportReadOnlyTexture(graph, bindings, *atlas->GetResource(),
                                                               published, "renderer particle atlas");
                particleAccess.push_back({id, Mode::Read, published});
            }
    packet("particle cull all groups/global SortedAlpha radix", particleAccess,
        [&particles](auto* commands) { particles.PrepareRenderData(commands); });
    const auto transparency = [&](GDX12WBOITTransparencyPass& pass, const char* name) {
        auto* accum = pass.GetOutput(0)->GetTexture(); auto* reveal = pass.GetOutput(1)->GetTexture();
        auto accesses = global;
        for (auto* input : pass._inputs) accesses.push_back(textureRead(input->GetTexture()));
        for (UINT index : {0u, 2u, 3u, 6u, 7u})
            accesses.push_back({resource(scene.Buffers[index].Data.Get()), Mode::Read, 0});
        accesses.push_back({resource(scene.FrameBuffers[0].Data.Get()), Mode::Read, 0});
        accesses.push_back({resource(scene.FrameBuffers[1].Data.Get()), Mode::Read, 0});
        accesses.push_back({resource(scene.Geometry.VertexResource), Mode::Read, 0});
        accesses.push_back({resource(scene.Geometry.IndexResource), Mode::Read, 0});
        auto a = textureRead(accum); a.Usage = Mode::Write; accesses.push_back(a);
        a = textureRead(reveal); a.Usage = Mode::Write; accesses.push_back(a);
        auto* root = pass._transparencyRS.get(); auto pso = pass._transparencyPSO;
        auto signature = pass._transparencyCS;
        const auto geom = scene.Geometry;
        const auto main = scene.Main, camera = scene.Camera;
        const auto shadow = frame.ShadowCB->GetElementAddress(0), sky = frame.SkyCB->GetElementAddress(0);
        const auto depthView = scene.ReadDepth.CPUHandle;
        const auto maxDraws = scene.Draws;
        auto* args = scene.Buffers[6].Data.Get(); auto* count = scene.Buffers[7].Data.Get();
        std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 17> srv{};
        srv[0] = scene.Buffers[2].Srv.GPUHandle; srv[1] = scene.Buffers[3].Srv.GPUHandle;
        srv[2] = scene.Buffers[0].Srv.GPUHandle;
        for (UINT i = 1; i < 5; ++i) srv[2+i] = pass._inputs[i]->GetTexture()->GetSRV()->GPUHandle;
        srv[7] = frame.ShadowLights->GetSRV()->GPUHandle;
        srv[8] = pass._inputs[5]->GetTexture()->GetSRV()->GPUHandle;
        srv[9] = _resources.ReflectionRadianceSRV.GPUHandle;
        srv[10] = frame.ReflectionProbes->GetSRV()->GPUHandle;
        srv[11] = _resources.SkyTransmittanceSRV.GPUHandle;
        srv[12] = _resources.SkyAerialRadianceSRV.GPUHandle;
        srv[13] = _resources.SkyAerialTransmittanceSRV.GPUHandle;
        srv[14] = _depth->GetSRV()->GPUHandle; srv[15] = opaqueDepth->GetSRV()->GPUHandle;
        srv[16] = _resources.SRV_UAV_Heap->GetGPUHandle(Texture2D_StartIndex);
        auto* heap = _resources.SRV_UAV_Heap.get();
        // Exactly one clear at each far/near boundary. Particle producers append
        // to the near accumulators and never clear them per emitter/group.
        packet(name, accesses, [=](auto* commands) {
            commands->SetDescriptorHeaps({heap});
            commands->ResourceBarrier({opaqueDepth->GetResource()->GetTransitionBarrier(
                D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
                accum->GetResource()->GetRenderTargetBarrier(), reveal->GetResource()->GetRenderTargetBarrier()});
            commands->ClearRenderTargetView(accum); commands->ClearRenderTargetView(reveal);
            commands->SetViewport(accum->GetViewport()); commands->SetScissorRect(accum->GetScissorRect());
            const D3D12_CPU_DESCRIPTOR_HANDLE targets[]{accum->GetRTV()->CPUHandle, reveal->GetRTV()->CPUHandle};
            auto* cmd = commands->GetCommandList().Get();
            cmd->OMSetRenderTargets(2, targets, FALSE, &depthView);
            commands->SetGraphicsRootSignature(root); commands->SetPipelineState(pso);
            commands->SetGraphicsRootConstantBufferView(1, main); commands->SetGraphicsRootConstantBufferView(2, camera);
            commands->SetGraphicsRootConstantBufferView(3, shadow); commands->SetGraphicsRootConstantBufferView(4, sky);
            for (UINT i = 0; i < srv.size(); ++i) commands->SetGraphicsSRV(i, srv[i]);
            const auto geometryRoot = root->GetGeometryRootParamIndex();
            cmd->SetGraphicsRootShaderResourceView(UINT(geometryRoot), geom.Vertices.BufferLocation);
            // TRANSPARENT_UNBATCHED reads the command's root instance directly;
            // t1/space1 (VisibleSkinInstances) is unused and is not bound to vertices.
            cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            cmd->IASetVertexBuffers(0, 1, &geom.Vertices); cmd->IASetIndexBuffer(&geom.Indices);
            // Buffers enter GENERIC_READ, including INDIRECT_ARGUMENT.
            if (maxDraws) cmd->ExecuteIndirect(signature.Get(), maxDraws, args, 0, count, 0);
        });
    };
    const auto compose = [&](GDX12WBOITCompositionPass& pass, const char* name) {
        auto accesses = global;
        for (auto* input : pass._inputs) accesses.push_back(textureRead(input->GetTexture()));
        auto a = textureRead(pass.GetOutput(0)->GetTexture()); a.Usage = Mode::Write; accesses.push_back(a);
        packet(name, accesses, [&pass](auto* commands) { pass.Execute(commands); });
    };
    transparency(path.Far, "far mesh transparency clear/draw");
    compose(path.FarComposition, "far WBOIT resolve/opaque aerial once");
    auto resolveAccess = waterAccess;
    resolveAccess.push_back(textureRead(path.FarComposition.GetOutput(0)->GetTexture()));
    auto* background = path.FarComposition.GetOutput(0)->GetTexture();
    auto* environmentMap = environment.Output(1)->GetTexture();
    packet("water optics/refraction/probe resolve", resolveAccess, [&water, background, environmentMap](auto* commands) {
        water.Resolve(commands, background, environmentMap);
    });
    transparency(path.Near, "near mesh transparency clear/draw");
    particleAccess.push_back(textureRead(path.Near.GetOutput(0)->GetTexture()));
    particleAccess.back().Usage = Mode::ReadWrite;
    particleAccess.push_back(textureRead(path.Near.GetOutput(1)->GetTexture()));
    particleAccess.back().Usage = Mode::ReadWrite;
    packet("particle weighted-alpha append/feature motion", particleAccess,
        [&particles](auto* commands) { particles.DrawWeightedAndMotion(commands); });
    compose(path.NearComposition, "near WBOIT resolve without repeated opaque aerial");
    auto* output = path.NearComposition.GetOutput(0)->GetTexture();
    particleAccess.push_back(textureRead(output)); particleAccess.back().Usage = Mode::ReadWrite;
    packet("global SortedAlpha/additive HDR composition", particleAccess,
        [&particles, output](auto* commands) { particles.Composite(commands, output); });
    // Restore the App's simulation owner after preparation. Each packet briefly
    // installs this path under owner-only recording, with exception restoration.
    return output;
}
}

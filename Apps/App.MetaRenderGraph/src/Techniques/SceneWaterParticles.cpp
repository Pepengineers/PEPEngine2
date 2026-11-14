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
    Path() {
        Far.SetUnbatchedDraws(true); Near.SetUnbatchedDraws(true);
        Water.SetLocalExecution(true); Particles.SetLocalExecution(true);
        NearComposition.SetApplyOpaqueAerialPerspective(false);
        for(auto* pass:std::array<GDX12RenderPass*,4>{&Far,&Near,&FarComposition,&NearComposition})
            pass->SetFlag(RENDER_PASS_FLAG_USE_DOWNSCALED_RESOLUTION,true);
    }
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
std::vector<GDX12Texture*> SceneWaterParticles::DiagnosticImages(bool serial) const
{
    auto& path=*_paths[serial?0:1];
    auto* farHDR=path.FarComposition.GetOutput(0)->GetTexture();
    return {farHDR,path.Water.NativeColor()?path.Water.NativeColor():farHDR,
        path.Near.GetOutput(0)->GetTexture(),path.Near.GetOutput(1)->GetTexture(),path.Particles.MotionOutput()};
}
std::vector<std::pair<std::string,GDX12Resource*>> SceneWaterParticles::DiagnosticBuffers(bool serial) const
{
    auto& path=*_paths[serial?0:1];
    std::vector<std::pair<std::string,GDX12Resource*>> result;
    for(const auto& emitter:_resources.ParticleEmitters) {
        const std::vector<uint64_t> ids{emitter.Id};
        const auto buffers=path.Simulation->GetPoolResources(&ids);
        const char* names[]={"particles","alive0","alive1","dead","counters","args"};
        for(size_t i=0;i<buffers.size();++i) result.emplace_back("emitter-"+std::to_string(emitter.Id)+"-"+names[i],buffers[i]);
    }
    const auto buffers=path.Particles.NativeResources();
    for(size_t i=0;i<buffers.size();++i)
        if(buffers[i]->D3DResource->GetDesc().Dimension==D3D12_RESOURCE_DIMENSION_BUFFER)
            result.emplace_back("render-buffer-"+std::to_string(i),buffers[i]);
    return result;
}
GDX12FluidSurfacePass::SimulationWork SceneWaterParticles::SimulationWork() const { return _paths[1]->Water.LastSimulationWork(); }
std::vector<std::pair<std::string,GDX12Resource*>> SceneWaterParticles::WaterBuffers() const {
    std::vector<std::pair<std::string,GDX12Resource*>> result;
    for(const auto& a:_paths[1]->Water.GetSimulationBufferAccesses())
        if(a.Resource->D3DResource->GetDesc().Dimension==D3D12_RESOURCE_DIMENSION_BUFFER)
            result.emplace_back("water-"+std::to_string(result.size()),a.Resource);
    return result;
}
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
    GDX12Texture* probes, D3D12_GPU_DESCRIPTOR_HANDLE probeRadiance, bool serial)
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
    const auto packet = [&](const std::string& name, std::vector<Access> accesses,
                            std::function<void(GDX12CommandList*)> record, Queue target = Queue::Direct) {
        std::vector<Access> unique;
        for (auto a : accesses)
        {
            // All packet boundaries are explicit read states. Private transitions
            // return to that state; DEFAULT-buffer decay is executor-owned.
            a.Required = readState(bindings.at(a.Resource));
            if (target == Queue::Compute)
            {
                D3D12_HEAP_PROPERTIES heap{};
                ThrowIfFailed(bindings.at(a.Resource)->GetHeapProperties(&heap, nullptr));
                if (heap.Type == D3D12_HEAP_TYPE_DEFAULT && a.Required != D3D12_RESOURCE_STATE_COMMON)
                    a.Required = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            }
            auto found = std::find_if(unique.begin(), unique.end(), [&](auto b) { return a.Resource == b.Resource; });
            if (found == unique.end()) unique.push_back(a);
            else if (a.Usage != Mode::Read) found->Usage = Mode::ReadWrite;
        }
        std::vector<GDX12RecordingStates::Binding> states;
        for (auto a : unique) states.emplace_back(bindings.at(a.Resource), D3D12_RESOURCE_STATES(a.Required));
        const Id id = _work.Add(graph, name, unique, [this, &path, states, record](auto* commands) {
            GDX12RecordingStates tracker(states);
            GDX12CommandList borrowed(commands);
            // Original render helpers look up the active primary simulation.
            // Install only this diagnostic path during its owner callback.
            SimulationBinding active(_resources.ParticleSimulation, path.Simulation);
            try { record(&borrowed); }
            catch (...) { _reset = true; throw; }
            if (borrowed.HasSubmissionSignals()) throw std::logic_error("U6 cannot publish private submission signals");
            tracker.Finish(commands);
        });
        graph.Operations[id].OwnerRecording = true;
        graph.Operations[id].Target = target;
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
    auto* opaqueDepth = scene.DepthView.get();
    std::array<std::vector<Access>, 2> groupPools;
    for (size_t group = 0; group < _groups.size(); ++group)
    {
        const auto ids = _groups[group].Emitters;
        appendResources(groupPools[group], simulation->GetPoolResources(&ids), Mode::Read);
        if (ids.empty()) continue;
        auto simAccess = groupPools[group];
        for (auto& a : simAccess) a.Usage = Mode::ReadWrite;
        const auto initAccess = simAccess;
        appendResources(simAccess, simulation->GetConstantResources(slot, &ids), Mode::Read);
        simAccess.push_back(read(&frame.CameraCB->GetResource()));
        if (_groups[group].DepthCollision) simAccess.push_back(textureRead(opaqueDepth));
        if (auto* interaction = simulation->GetInteractionResource()) simAccess.push_back(read(interaction));
        const auto target = !serial && group == 1 ? Queue::Compute : Queue::Direct;
        const auto name = group == 0 ? "particle group A " : "particle group B ";
        packet(std::string(name) + "pool initialization/reset", initAccess,
            [this, simulation, ids](auto* commands) { simulation->InitializePools(commands, &_resources, &ids); }, target);
        packet(std::string(name) + "simulate/death/spawn/burst/publish", simAccess,
            [this, simulation, opaqueDepth, ids](auto* commands) {
                simulation->Execute(commands, &_resources, _common, opaqueDepth, &ids);
            }, target);
    }
    // Shared draw buffers stay on DIRECT. The first cull reads only A's pools;
    // B is joined at its append, then all SortedAlpha keys are sorted together.
    std::vector<Access> particleAccess{read(&frame.CameraCB->GetResource())};
    appendResources(particleAccess, particles.NativeResources(), Mode::ReadWrite);
    const auto preparationAccess = particleAccess;
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
    const auto prepareGroup = [&](size_t group) {
        auto accesses = preparationAccess;
        accesses.insert(accesses.end(), groupPools[group].begin(), groupPools[group].end());
        const auto ids = _groups[group].Emitters;
        packet(group == 0 ? "particle group A clear/cull" : "particle group B append/global SortedAlpha radix", accesses,
            [&particles, ids, group](auto* commands) { particles.PrepareRenderGroup(commands, ids, group == 0, group == 1); });
    };
    prepareGroup(0);
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
        srv[9] = probeRadiance;
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
    packet("water optics/refraction/probe resolve", resolveAccess, [&water, background, environmentMap, probeRadiance](auto* commands) {
        water.Resolve(commands, background, environmentMap, probeRadiance);
    });
    transparency(path.Near, "near mesh transparency clear/draw");
    prepareGroup(1);
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

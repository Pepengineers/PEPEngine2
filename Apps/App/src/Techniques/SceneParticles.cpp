#include "SceneParticles.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12TextureResource.h"
#include "Engine.RendererDX12/GDX12MotionBlurSettings.h"
#include "Engine.RendererDX12/GDX12FrameGeneration.h"

namespace Meta
{
namespace
{
State BoundaryState(ID3D12Resource* resource)
{
    const auto desc = resource->GetDesc();
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) return D3D12_RESOURCE_STATE_GENERIC_READ;
    return desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE ? State(D3D12_RESOURCE_STATE_COMMON) :
        State(D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
}
Access ResourceAccess(Graph& graph, std::vector<ID3D12Resource*>& bindings, GDX12Resource* resource, Mode mode,
    State state = State(-1))
{
    auto* native = resource->D3DResource.Get();
    D3D12_HEAP_PROPERTIES heap{};
    ThrowIfFailed(native->GetHeapProperties(&heap, nullptr));
    return {NativeWork::ResourceFor(graph, bindings, native, "particle resource"),
        heap.Type == D3D12_HEAP_TYPE_UPLOAD ? Mode::Read : mode,
        heap.Type == D3D12_HEAP_TYPE_UPLOAD || state == State(-1) ? BoundaryState(native) : state};
}
void AddResources(Graph& graph, std::vector<ID3D12Resource*>& bindings, std::vector<Access>& accesses,
    const std::vector<GDX12Resource*>& resources, Mode mode)
{
    for (auto* resource : resources) accesses.push_back(ResourceAccess(graph, bindings, resource, mode));
}
void AddAccesses(Graph& graph, std::vector<ID3D12Resource*>& bindings, std::vector<Access>& accesses,
    const std::vector<GDX12ResourceAccess>& resources)
{
    for (const auto& resource : resources)
        accesses.push_back(ResourceAccess(graph, bindings, resource.Resource,
            resource.Usage == GDX12AccessMode::Read ? Mode::Read :
            resource.Usage == GDX12AccessMode::Write ? Mode::Write : Mode::ReadWrite, resource.State));
}
}
SceneParticles::SceneParticles(GDX12DeviceResources& resources, RenderPipelineCommonData& common)
    : _resources(resources), _common(common)
{
    if (!_resources.ParticleSimulation)
        _resources.ParticleSimulation = std::make_unique<GDX12ParticleSimulation>();
    // Merged fluid depth is an SRV/UAV, so the raster PSO always uses shader depth rejection.
    _pass.SetReadOnlyDepthEnabled(false);
    _pass.SetMotionOutputEnabled(false);
    _pass.Setup(&_resources, &_common);
}
SceneParticles::~SceneParticles()
{
    _pass.ClearDenendencies();
    if (_resources.ParticleSimulation) _resources.ParticleSimulation->SetInteractionField({});
}
void SceneParticles::Packet(Graph& graph, const std::vector<ID3D12Resource*>& bindings, const char* name,
    std::vector<Access> accesses, std::function<void(GDX12CommandList*)> record)
{
    std::vector<Access> unique;
    for (auto access : accesses)
    {
        const auto found = std::find_if(unique.begin(), unique.end(), [&](auto other) {
            return other.Resource == access.Resource;
        });
        if (found == unique.end()) unique.push_back(access);
        else
        {
            if (found->Required != access.Required) throw std::logic_error("Conflicting particle resource states");
            if (found->Usage != access.Usage) found->Usage = Mode::ReadWrite;
        }
    }
    std::vector<GDX12RecordingStates::Binding> states;
    for (const auto& access : unique)
        states.emplace_back(bindings.at(access.Resource), D3D12_RESOURCE_STATES(access.Required));
    const Id operation = _work.Add(graph, name, std::move(unique), [states, record](auto* native) {
        GDX12RecordingStates tracker(states);
        GDX12CommandList borrowed(native);
        record(&borrowed);
        if (borrowed.HasSubmissionSignals())
            throw std::logic_error("Meta particles cannot submit private signals");
        tracker.Finish(native);
    });
    graph.Operations.at(operation).OwnerRecording = true;
    graph.Operations.at(operation).Target = Queue::Direct;
}
void SceneParticles::Prepare(Graph& graph, std::vector<ID3D12Resource*>& bindings,
    GDX12Texture* simulationDepth, GDX12Texture* rasterDepth, GDX12Texture* accum, GDX12Texture* reveal,
    GDX12ParticleInteractionField interaction)
{
    _work.Commands.clear();
    _drawReads.clear();
    _accum = accum;
    _reveal = reveal;
    if (_width && (_width != rasterDepth->GetWidth() || _height != rasterDepth->GetHeight())) _pass.Resize();
    _width = rasterDepth->GetWidth();
    _height = rasterDepth->GetHeight();
    auto* simulation = _resources.ParticleSimulation.get();
    simulation->SetInteractionField(interaction);
    simulation->BeginFrame(&_resources, _common);
    _pass.SetInputs({rasterDepth, accum, reveal});
    _pass.SetMotionOutputEnabled(GetMotionBlurSettings().Enabled ||
        GetFrameGenerationProvider() != GDX12FrameGenerationProvider::Off ||
        (_common.Upscaler && (_common.Upscaler->GetFlags() & RENDER_PASS_FLAG_USE_JITTER)));
    _pass.Initialize();
    _pass.BeginFrame();
    const UINT slot = _resources.CurrFrameConstantsIndex;
    std::array<std::vector<uint64_t>, 2> groups;
    for (const auto& emitter : _resources.ParticleEmitters)
        groups[emitter.DepthCollision ? 1 : 0].push_back(emitter.Id);
    const auto camera = ResourceAccess(graph, bindings,
        &_resources.FrameConstants.at(slot)->CameraCB->GetResource(), Mode::Read);
    std::array<std::vector<Access>, 2> pools;
    for (size_t group = 0; group < groups.size(); ++group)
    {
        const auto ids = groups[group];
        AddResources(graph, bindings, pools[group], simulation->GetPoolResources(&ids), Mode::Read);
        if (ids.empty()) continue;
        auto initialization = pools[group];
        for (auto& access : initialization) access.Usage = Mode::ReadWrite;
        Packet(graph, bindings, group == 0 ? "particle A pool initialization/reset" : "particle B pool initialization/reset",
            initialization, [this, simulation, ids](auto* cmd) { simulation->InitializePools(cmd, &_resources, &ids); });
        auto simulationAccess = initialization;
        AddResources(graph, bindings, simulationAccess, simulation->GetConstantResources(slot, &ids), Mode::Read);
        simulationAccess.push_back(camera);
        if (group == 1) simulationAccess.push_back(ResourceAccess(graph, bindings, simulationDepth->GetResource(), Mode::Read));
        if (interaction.Resource) simulationAccess.push_back(ResourceAccess(graph, bindings, interaction.Resource, Mode::Read));
        Packet(graph, bindings, group == 0 ? "particle A simulate/death/spawn/burst/publish" : "particle B simulate/death/spawn/burst/publish",
            std::move(simulationAccess), [this, simulation, simulationDepth, ids](auto* cmd) {
                simulation->Execute(cmd, &_resources, _common, simulationDepth, &ids);
            });
    }
    std::vector<Access> preparation{camera};
    AddAccesses(graph, bindings, preparation, _pass.NativePreparationAccesses());
    for (size_t group = 0; group < groups.size(); ++group)
    {
        auto accesses = preparation;
        accesses.insert(accesses.end(), pools[group].begin(), pools[group].end());
        const auto ids = groups[group];
        Packet(graph, bindings, group == 0 ? "particle A clear/cull" : "particle B append/global SortedAlpha radix",
            std::move(accesses), [this, ids, group](auto* cmd) { _pass.PrepareRenderGroup(cmd, ids, group == 0, group == 1); });
    }
    _drawReads = {camera};
    _drawReads.push_back(ResourceAccess(graph, bindings, rasterDepth->GetResource(), Mode::Read));
    if (!_pass.NativeRasterAccesses(false).empty())
        for (const auto& emitter : _resources.ParticleEmitters)
            if (emitter.Texture)
            {
                auto* atlas = emitter.Texture->PrimaryDeviceTexture;
                if (!atlas) throw std::logic_error("Particle atlas has no primary texture");
                constexpr State published = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
                const Id id = NativeWork::ImportReadOnlyTexture(graph, bindings, *atlas->GetResource(), published,
                    "renderer particle atlas");
                _drawReads.push_back({id, Mode::Read, published});
            }
}
void SceneParticles::AppendWeighted(Graph& graph, std::vector<ID3D12Resource*>& bindings)
{
    auto accesses = _drawReads;
    AddAccesses(graph, bindings, accesses, _pass.NativeRasterAccesses(true));
    accesses.push_back(ResourceAccess(graph, bindings, _accum->GetResource(), Mode::ReadWrite, D3D12_RESOURCE_STATE_RENDER_TARGET));
    accesses.push_back(ResourceAccess(graph, bindings, _reveal->GetResource(), Mode::ReadWrite, D3D12_RESOURCE_STATE_RENDER_TARGET));
    Packet(graph, bindings, "particle weighted-alpha append", std::move(accesses),
        [this](auto* cmd) { _pass.DrawWeightedAndMotion(cmd); });
}
void SceneParticles::AppendComposite(Graph& graph, std::vector<ID3D12Resource*>& bindings, GDX12Texture* hdr)
{
    auto accesses = _drawReads;
    AddAccesses(graph, bindings, accesses, _pass.NativeRasterAccesses(false));
    accesses.push_back(ResourceAccess(graph, bindings, hdr->GetResource(), Mode::ReadWrite, D3D12_RESOURCE_STATE_RENDER_TARGET));
    Packet(graph, bindings, "global SortedAlpha/additive HDR particles", std::move(accesses),
        [this, hdr](auto* cmd) { _pass.Composite(cmd, hdr); });
}
}

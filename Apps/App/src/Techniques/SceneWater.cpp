#include "SceneWater.h"
#include "Engine.Core/BenchmarkEngine.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12TextureResource.h"

namespace Meta
{
SceneWater::SceneWater(GDX12DeviceResources& resources, RenderPipelineCommonData& common)
    : _resources(resources), _common(common),
      _collector(std::make_unique<BistroWaterRendering>(BenchmarkEngine::GetLocator().GetModule<SceneManagerModule>()))
{
    _surface.SetStaticIBL(true);
    _surface.SetParticleRendering(true);
    _surface.Setup(&_resources, &_common);
}
SceneWater::~SceneWater() { _surface.ClearDenendencies(); }
void SceneWater::SetRetirement(std::function<void()> retire)
{
    _retire = std::move(retire);
    _surface.SetResourceRetirement(_retire);
}
void SceneWater::Prepare(float delta)
{
    _surface.SetInstances(_collector->CollectScene(delta));
}
void SceneWater::Packet(Graph& graph, std::vector<ID3D12Resource*>& bindings, const char* name,
    const std::vector<GDX12ResourceAccess>& resources, std::function<void(GDX12CommandList*)> record)
{
    std::vector<Access> accesses;
    for (const auto& resource : resources)
    {
        auto* native = resource.Resource->D3DResource.Get();
        const Id id = NativeWork::ResourceFor(graph, bindings, native, name);
        const bool upload = graph.Resources.at(id).Memory == Heap::Upload;
        const auto mode = upload || resource.Usage == GDX12AccessMode::Read ? Mode::Read :
            resource.Usage == GDX12AccessMode::Write ? Mode::Write : Mode::ReadWrite;
        const State state = upload ? State(D3D12_RESOURCE_STATE_GENERIC_READ) :
            resource.State != D3D12_RESOURCE_STATES(-1) ? State(resource.State) :
            native->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ?
                State(mode == Mode::Read ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_UNORDERED_ACCESS) :
                State(D3D12_RESOURCE_STATE_COMMON);
        const auto found = std::find_if(accesses.begin(), accesses.end(), [id](auto access) { return access.Resource == id; });
        if (found == accesses.end()) accesses.push_back({id, mode, state});
        else
        {
            if (found->Required != state) throw std::logic_error("Conflicting water packet states");
            if (found->Usage != mode) found->Usage = Mode::ReadWrite;
        }
    }
    std::vector<GDX12RecordingStates::Binding> states;
    for (const auto& access : accesses)
        states.emplace_back(bindings.at(access.Resource), D3D12_RESOURCE_STATES(access.Required));
    const Id id = _work.Add(graph, name, std::move(accesses), [states, record](auto* commands) {
        GDX12RecordingStates tracker(states);
        GDX12CommandList borrowed(commands);
        record(&borrowed);
        if (borrowed.HasSubmissionSignals()) throw std::logic_error("Water recording cannot submit private queue signals");
        tracker.Finish(commands);
    });
    graph.Operations.at(id).OwnerRecording = true;
    graph.Operations.at(id).Target = Queue::Direct;
}
void SceneWater::AppendSurface(Graph& graph, std::vector<ID3D12Resource*>& bindings,
    GDX12Texture* opaqueDepth, GDX12Texture* opaqueMotion)
{
    if (!_retire) throw std::logic_error("Water requires executor-owned resource retirement");
    _work.Commands.clear();
    _surface.SetInputs({opaqueDepth, opaqueMotion});
    _surface.Initialize();
    const UINT width = opaqueDepth->GetWidth(), height = opaqueDepth->GetHeight();
    if (_width && (_width != width || _height != height))
    {
        _retire();
        _surface.Resize();
    }
    _width = width; _height = height;
    _surface.PrepareFrame();
    _depth = _surface.GetOutput(0)->GetTexture();
    _motion = _surface.GetOutput(1)->GetTexture();
    if (_surface.NeedsSimulation())
        Packet(graph, bindings, "water seed / SPH hash-sort-density-integrate",
            _surface.GetSimulationBufferAccesses(), [this](auto* commands) { _surface.ExecuteSimulation(commands); });
}
void SceneWater::AppendDraw(Graph& graph, std::vector<ID3D12Resource*>& bindings,
    GDX12Texture* hdr, D3D12_CPU_DESCRIPTOR_HANDLE depth)
{
    if (!_surface.IsRendering()) return;
    auto accesses = _surface.NativeSurfaceAccesses();
    accesses.push_back({hdr->GetResource(), GDX12AccessMode::ReadWrite, D3D12_RESOURCE_STATE_RENDER_TARGET});
    accesses.push_back({_depth->GetResource(), GDX12AccessMode::ReadWrite, D3D12_RESOURCE_STATE_DEPTH_WRITE});
    Packet(graph, bindings, "water draw / MGPU unlit opaque particles", accesses,
        [this, hdr, depth](auto* commands) { _surface.DrawParticles(commands, hdr, depth); });
}
}

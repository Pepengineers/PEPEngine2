#include "SceneReflections.h"
#include "ProbeInputMirror.h"
#include "ProbeTransfer.h"
#include "Engine.Core/Logging/Logger.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include "Common/GameTimer.h"
#include <cstring>
namespace BistroRender
{
namespace
{
AutoConsoleVariable<int> TransferMode(L"App.ReflectionProbes.TransferMode", 0,
    L"Remote transfer schedule: 0 all ready cubemaps, 1 one cubemap, 2 one face per frame",
    EConsoleVariableFlags::Default, [](int value) { return value >= 0 && value <= 2; });
}
struct SceneReflections::CaptureInputs
{
    std::array<Id, 4> Scene{};
    std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 4> SceneViews{};
    Id Vertex = Invalid, Index = Invalid;
    GDX12GeometryBuffer::RecordingBinding Geometry{};
    UINT Draws = 0, Batches = 0, Instances = 0;
    D3D12_GPU_VIRTUAL_ADDRESS Main = 0, Sky = 0;
    D3D12_GPU_DESCRIPTOR_HANDLE Lights{};
    std::array<GDX12Texture*, 4> Environment{};
    GDX12Texture* StaticShadows = nullptr;
    GDX12Texture* DynamicShadows = nullptr;
    std::vector<Access> Reads;
    std::function<void(const GeometryTarget&)> Vegetation;
};

struct SceneReflections::Remote
{
    struct Generation
    {
        std::unique_ptr<ProbeTransfer> Transfer;
        GDX12ReflectionProbe Source;
        CompletionToken Captured, Imported;
        CompletionToken FirstMaterialFrame;
        Id ImportOperation = Invalid;
        UINT Faces = 0;
        uint64_t Number = 0;
        uint64_t StaticGeometryRevision = 0;
        bool Filtered = false, Publish = false, Reported = false;
    };

    Remote(const std::shared_ptr<QueueSubmission>& submissions, Id id, UINT slots)
        : Device(id), GPU(submissions, id, slots), Mirror(*submissions, id)
    {
        Resources.Initialize(&submissions->Device(id));
        Pass.Setup(&Resources, &Common);
        Pass.Initialize();
        Vegetation = std::make_unique<SceneGeometry::RemoteProbeVegetation>(Resources);
        auto* heap = Resources.SRV_UAV_Heap.get();
        GDX12TextureDesc shadow{};
        shadow.Width = shadow.Height = 1;
        shadow.Format = DXGI_FORMAT_R32_FLOAT;
        shadow.SRV_UAV_Heap = heap;
        shadow.SRVHeapIndex = heap->GetAvailableIndex(SceneDescriptorStart);
        shadow.SRVDesc.Format = shadow.Format;
        shadow.SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        shadow.SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        shadow.SRVDesc.Texture2DArray.MipLevels = shadow.SRVDesc.Texture2DArray.ArraySize = 1;
        ShadowFallback.Initialize(shadow);
        Lights = std::make_unique<GDX12UploadBuffer<GDX12LocalLightConstants>>(Resources.Device, 1, EBufferType::Upload, false);
        Lights->CreateSRV(heap, heap->GetAvailableIndex(SceneDescriptorStart));
    }

    ~Remote()
    {
        try { GPU.Drain(); }
        catch (...) {}
        Pass.ClearDenendencies();
    }

    Id Device;
    Executor GPU;
    ProbeInputMirror Mirror;
    GDX12DeviceResources Resources;
    RenderPipelineCommonData Common;
    GDX12ReflectionProbePass Pass;
    std::unique_ptr<SceneGeometry::RemoteProbeVegetation> Vegetation;
    GDX12Texture ShadowFallback;
    std::unique_ptr<GDX12UploadBuffer<GDX12LocalLightConstants>> Lights;
    std::array<GDX12Descriptor, 4> SceneViews;
    std::array<GDX12Texture, 4> Environment;
    std::array<GDX12ResourceHandle, 4> EnvironmentResources;
    NativeWork Work;
    Graph Plan;
    std::vector<ID3D12Resource*> Bindings;
    std::map<UINT, Generation> Probes;
    std::vector<UINT> Indices, Filtered;
    std::vector<Id> PrimaryImports;
    CompletionToken LastWork;
    Id PrimaryExport = Invalid, RemoteImport = Invalid;
    uint64_t EnvironmentRevision = UINT64_MAX, TextureRevision = UINT64_MAX, MeshRevision = UINT64_MAX;
    bool WorkPlanned = false;
};

SceneReflections::SceneReflections(GDX12DeviceResources& r, RenderPipelineCommonData& c) : _resources(r), _common(c)
{

    {
        auto& p = _passes;
        p = std::make_unique<GDX12ReflectionProbePass>();
        p->Setup(&r, &c);
        p->Initialize();
        p->CreatePublishedTexture(1, 1);
        _black.D3DResource = r.Device->CreateResource(D3D12_HEAP_TYPE_UPLOAD, CD3DX12_RESOURCE_DESC::Buffer(256),
                                                            D3D12_RESOURCE_STATE_GENERIC_READ);
        void* data = nullptr;
        const D3D12_RANGE noRead{};
        ThrowIfFailed(_black.D3DResource->Map(0, &noRead, &data));
        std::memset(data, 0, 256);
        static_cast<uint16_t*>(data)[3] = 0x3c00;
        _black.D3DResource->Unmap(0, nullptr);
    }
}
SceneReflections::~SceneReflections()
{
    _passes->ClearDenendencies();
}
void SceneReflections::Reset()
{

    {
        for (auto& [id, remote] : _remotes) remote->GPU.Drain();
        if (!_remotes.empty()) _resources.Device->GetCommandQueue()->Flush();
        _remotes.clear();
        _owners.clear();
        _transferProbe = 0;
        _passes->ResetHistory();
        _publications.clear();
    }
}
bool SceneReflections::Record(Id id, ID3D12GraphicsCommandList* cmd) const
{
    return _work.Record(id, cmd);
}
void SceneReflections::Submitted(uint64_t fence)
{
    if (!fence)
        throw std::invalid_argument("Probe publication requires a submitted fence");
    for (auto probe : _publishing)
        _publications.at(probe).Fence = fence;
}
std::pair<size_t, size_t> SceneReflections::Readiness() const
{
    const auto& p = *_passes;
    if (!p._enabled)
        return {};
    const auto done = _resources.Device->GetCommandQueue()->GetFence()->GetCompletedValue();
    if (done == UINT64_MAX)
        throw std::runtime_error("Probe completion fence reports device removal");
    size_t ready = 0;
    for (const auto& publication : _publications)
        ready += publication.Ready || (publication.Fence && publication.Fence <= done);
    return {ready, p._probes.size()};
}
void SceneReflections::Append(Graph& graph, std::vector<ID3D12Resource*>& bindings, SceneGeometry& geometry,
                              SceneEnvironment& environment, UINT slot, GDX12Texture* localShadows,
                              GDX12Texture* staticLocalShadows)
{
    auto& p = *_passes;
    auto& work = _work;
    work.Commands.clear();
    _publishing.clear();
    auto& scene = geometry._slots.at(slot);
    p._externalSettings = &_settings;
    std::vector<Id> owners;
    if (_settings.Enabled)
        for (const auto& probe : _resources.ReflectionProbes)
            owners.push_back(_submissions->SelectAdapter(probe.Adapter));
    if (_owners != owners)
    {
        for (auto& [id, remote] : _remotes) remote->GPU.Drain();
        if (!_remotes.empty()) _resources.Device->GetCommandQueue()->Flush();
        _remotes.clear();
        for (size_t i = 0; i < (std::min)(owners.size(), _owners.size()); ++i)
            if (owners[i] != _owners[i] && i < p._probes.size())
            {
                p._probes[i].Ready = false;
                p._probes[i].CapturedFaces = p._probes[i].BakedFaces = p._probes[i].NextFace = 0;
                p._probes[i].LastUpdate = 0;
            }
        _owners = std::move(owners);
        _transferProbe = 0;
    }
    p._captureAllowed = [this](UINT index) { return _owners.at(index) == 0; };
    if (!localShadows || !staticLocalShadows)
        throw std::invalid_argument("Ordinary probe capture requires explicit local shadow inputs");
    p._externalStaticShadows = staticLocalShadows;
    p._externalDynamicShadows = localShadows;
    p.SetInputs({environment.Output(0), environment.Output(1), environment.Output(2), environment.Output(3),
                 localShadows});
    const auto history = _common.HistoryRevision;
    const bool reset = _common.ResetHistory, upscalerReset = _common.ResetUpscalerHistory,
               requestReset = _common.RequestHistoryReset;
    _common.HistoryRevision = _historyBase;
    try
    {
        p.Prepare();
    }
    catch (...)
    {
        _common.HistoryRevision = history;
        _common.ResetHistory = reset;
        _common.ResetUpscalerHistory = upscalerReset;
        _common.RequestHistoryReset = requestReset;
        throw;
    }
    const bool replaced = p._publishedNeedsTransition;
    _history = _common.HistoryRevision;
    _common.HistoryRevision = history + (_history - _historyBase);

    if (_radiance.HeapIndex == UINT_MAX || replaced)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.TextureCubeArray.NumCubes = p._published.GetResource()->D3DResource->GetDesc().DepthOrArraySize / 6;
        view.TextureCubeArray.MipLevels = p._publishedMips;
        auto& descriptor = _radiance;
        descriptor.InitAsSRV(p._published.GetResource()->D3DResource.Get(), &view, _resources.SRV_UAV_Heap.get(),
                             descriptor.HeapIndex == UINT_MAX ? _resources.SRV_UAV_Heap->GetAvailableIndex(SceneDescriptorStart)
                                                              : descriptor.HeapIndex);
    }
    std::vector<Id> publicationOperations;
    auto& publications = _publications;
    const auto done = _resources.Device->GetCommandQueue()->GetFence()->GetCompletedValue();
    if (replaced)
        publications.assign(p._probes.size(), {});
    else
        publications.resize(p._probes.size());
    for (UINT i = 0; i < publications.size(); ++i)
    {
        auto& publication = publications[i];
        if (!p._probes[i].Ready)
            publication = {};
        else
            publication.Ready |= publication.Fence && publication.Fence <= done;
    }
    const auto resource = [&](GDX12Resource* value) {
        return NativeWork::ResourceFor(graph, bindings, value->D3DResource.Get(), "probe resource");
    };
    const auto texture = [&](GDX12Texture* value, Mode mode, State state) {
        return Access{resource(value->GetResource()), mode, state};
    };
    const State read = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    if (!p._enabled && p._publishedNeedsTransition && p._publishedResolution == 1)
    {
        auto* source = _black.D3DResource.Get();
        auto* target = p._published.GetResource()->D3DResource.Get();
        work.Add(graph, "initialize disabled probe fallback",
                 {{resource(&_black), Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ},
                  texture(&p._published, Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST)},
                 [source, target](auto* cmd) {
                     const D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{0, {DXGI_FORMAT_R16G16B16A16_FLOAT, 1, 1, 1, 256}};
                     const CD3DX12_TEXTURE_COPY_LOCATION src(source, fp);
                     for (UINT face = 0; face < 6; ++face)
                     {
                         const CD3DX12_TEXTURE_COPY_LOCATION dst(target, face);
                         cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                     }
                 });
    }
    p._publishedNeedsTransition = false;
    auto& constants = *_resources.FrameConstants[slot];
    if (p._capturePlanned)
    {
        CaptureInputs in;
        for (UINT i = 0; i < 4; ++i)
        {
            in.Scene[i] = i;
            in.SceneViews[i] = scene.Buffers[i].Srv.GPUHandle;
        }
        in.Vertex = 18;
        in.Index = 19;
        in.Geometry = scene.Geometry;
        in.Draws = scene.Draws;
        in.Batches = scene.Batches;
        in.Instances = scene.InstanceCount;
        in.Main = scene.Main;
        in.Sky = constants.SkyCB->GetElementAddress(0);
        in.Lights = constants.ShadowLights->GetSRV()->GPUHandle;
        in.StaticShadows = staticLocalShadows;
        in.DynamicShadows = localShadows;
        for (UINT i = 0; i < 4; ++i) in.Environment[i] = environment.Output(i)->GetTexture();
        in.Reads = environment.Reads(graph, bindings);
        in.Reads.push_back({scene.FrameIds[0], Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ});
        in.Reads.push_back({resource(&constants.SkyCB->GetResource()), Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ});
        in.Reads.push_back({resource(&constants.ShadowLights->GetResource()), Mode::Read, read});
        in.Vegetation = [&](const GeometryTarget& target) { geometry.AppendVegetationView(graph, bindings, slot, target); };
        AppendCapture(graph, bindings, work, p, _resources, in, publicationOperations, _publishing);
    }
    AppendRemote(graph, bindings, geometry, environment, slot, publicationOperations, replaced);
    for (UINT i = 0; i < MaxReflectionProbes; ++i)
    {
        GDX12ReflectionProbeData data;
        if (p._enabled && i < p._probes.size() && p._probes[i].Ready)
        {
            const auto& state = p._probes[i];
            const auto& source = state.Published;
            data.Position = source.Position;
            data.BoxMin = source.BoxMin;
            data.BoxMax = source.BoxMax;
            data.BlendDistance = source.BlendDistance;
            data.Intensity = source.Intensity;
            data.MaxMip = float(p._publishedMips - 1);
            data.MinMip = float(p._publishedMips - GDX12RenderHelpers::MipCount(source.Resolution));
        }
        constants.ReflectionProbesUpload->CopyData(i, data);
    }
    auto* source = constants.ReflectionProbesUpload->GetResource().D3DResource.Get();
    auto* target = constants.ReflectionProbes->GetResource().D3DResource.Get();
    auto* metadata = constants.ReflectionProbes->GetSRV();
    D3D12_SHADER_RESOURCE_VIEW_DESC probeView{};
    probeView.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    probeView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    probeView.Buffer.StructureByteStride = sizeof(GDX12ReflectionProbeData);
    probeView.Buffer.NumElements = p._enabled ? UINT(p._probes.size()) : 1;
    metadata->InitAsSRV(target, &probeView, _resources.SRV_UAV_Heap.get(), metadata->HeapIndex);
    work.Add(
        graph, "probe metadata upload (GPU ordered)",
        {{resource(&constants.ReflectionProbesUpload->GetResource()), Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ},
         {resource(&constants.ReflectionProbes->GetResource()), Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST}},
        [source, target](auto* cmd) {
            cmd->CopyBufferRegion(target, 0, source, 0, UINT64(MaxReflectionProbes) * sizeof(GDX12ReflectionProbeData));
        },
        publicationOperations);
    p._planPrepared = false;
}
void SceneReflections::AppendRemote(Graph& primary, std::vector<ID3D12Resource*>& primaryBindings,
                                    SceneGeometry& geometry, SceneEnvironment& environment, UINT slot,
                                    std::vector<Id>& publications, bool arrayReplaced)
{
    if (!_passes->_enabled) return;
    if (arrayReplaced && !_remotes.empty())
    {
        for (auto& [id, remote] : _remotes) remote->GPU.Drain();
        _resources.Device->GetCommandQueue()->Flush();
        _remotes.clear();
        _transferProbe = 0;
    }
    const auto& scene = geometry._slots.at(slot);
    const auto& constants = *_resources.FrameConstants[slot];
    const auto sourceProbes = _resources.ReflectionProbes;
    for (UINT index = 0; index < _owners.size(); ++index)
    {
        const Id id = _owners[index];
        if (!id) continue;
        auto& entry = _remotes[id];
        if (!entry) entry = std::make_unique<Remote>(_submissions, id, UINT(_resources.FrameConstants.size()));
        entry->Probes.try_emplace(index);
    }

    for (auto& [id, entry] : _remotes)
    {
        auto& remote = *entry;
        remote.WorkPlanned = false;
        remote.PrimaryExport = remote.RemoteImport = Invalid;
        remote.PrimaryImports.clear();
        remote.Filtered.clear();
        if (remote.LastWork && !_submissions->Complete(remote.LastWork)) continue;
        auto& r = remote.Resources;
        auto& p = remote.Pass;
        auto& graph = remote.Plan;
        auto& bindings = remote.Bindings;
        auto& work = remote.Work;
        graph = {};
        graph.Device = id;
        graph.FrameBoundary = false;
        bindings.clear();
        work.Commands.clear();
        remote.Mirror.Begin();
        remote.Indices.clear();
        r.ReflectionProbes.clear();
        for (auto& [index, generation] : remote.Probes)
        {
            const auto& source = _passes->_probes[index].Source;
            if (generation.FirstMaterialFrame && _submissions->Complete(generation.FirstMaterialFrame))
            {
                Log::Info("RenderGraph", "GPU0 completed remote reflection probe id=", generation.Source.Id,
                          " generation=", generation.Number, " owner=", _submissions->AdapterLabel(id),
                          " faces=6 mipsPerFace=", generation.Transfer->MipCount(),
                          " stagingBytes=", generation.Transfer->TransferBytes(),
                          " GPU0 copyFence=", generation.Imported.Value,
                          " materialFrameFence=", generation.FirstMaterialFrame.Value);
                generation.Reported = true;
                generation.FirstMaterialFrame = {};
            }
            if (generation.Imported && !generation.FirstMaterialFrame && _submissions->Complete(generation.Imported))
            {
                generation.Imported = {};
                if (generation.Publish) generation.Filtered = false;
            }
            const bool staticChanged = source.Implementation == 2 &&
                generation.StaticGeometryRevision != _resources.ReflectionStaticGeometryRevision;
            if ((!SameReflectionCapture(generation.Source, source) || staticChanged) && !generation.Imported)
            {
                generation.Filtered = generation.Publish = false;
                generation.Faces = 0;
                if (!generation.Transfer || generation.Source.Resolution != source.Resolution)
                    generation.Transfer = std::make_unique<ProbeTransfer>(*_submissions, id, source.Resolution);
                generation.Source = source;
                generation.StaticGeometryRevision = _resources.ReflectionStaticGeometryRevision;
            }
            remote.Indices.push_back(index);
            r.ReflectionProbes.push_back(sourceProbes[index]);
        }
        r.CurrFrameConstantsIndex = slot;
        r.ReflectionStaticRevision = _resources.ReflectionStaticRevision;
        r.ReflectionStaticGeometryRevision = _resources.ReflectionStaticGeometryRevision;
        r.ReflectionEnvironmentRevision = _resources.ReflectionEnvironmentRevision;
        r.Lighting = _resources.Lighting;
        r.Lighting.SunShadowFlags = 0;
        p._externalSettings = &_settings;
        p._frameNumber = _passes->_frameNumber - 1;
        p._captureAllowed = [&](UINT index) {
            const auto& generation = remote.Probes.at(remote.Indices[index]);
            return !generation.Filtered && !generation.Imported;
        };
        p.SetInputs({environment.Output(0), environment.Output(1), environment.Output(2), environment.Output(3),
                     &remote.ShadowFallback});
        p.Prepare();
        p._planPrepared = false;
        if (!p._capturePlanned) continue;

        CaptureInputs in;
        const auto mirror = [&](Id source, bool refresh) {
            return remote.Mirror.Mirror(primary, primaryBindings, primaryBindings.at(source), source, refresh);
        };
        const auto bind = [&](ID3D12Resource* resource) {
            return NativeWork::ResourceFor(graph, bindings, resource, "remote probe input");
        };
        auto* heap = r.SRV_UAV_Heap.get();
        constexpr UINT strides[]{sizeof(GDX12InstanceData), sizeof(GDX12IndirectDrawArgs),
                                 sizeof(GDX12MaterialConstants), sizeof(GDX12TransformConstants)};
        for (UINT i = 0; i < 4; ++i)
        {
            auto* local = mirror(i, true);
            in.Scene[i] = bind(local);
            auto& descriptor = remote.SceneViews[i];
            D3D12_SHADER_RESOURCE_VIEW_DESC view{};
            view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            view.Buffer.StructureByteStride = strides[i];
            view.Buffer.NumElements = UINT(scene.Buffers[i].Bytes / strides[i]);
            descriptor.InitAsSRV(local, &view, heap, descriptor.HeapIndex == UINT_MAX
                ? heap->GetAvailableIndex(SceneDescriptorStart) : descriptor.HeapIndex);
            in.SceneViews[i] = descriptor.GPUHandle;
        }
        in.Geometry = scene.Geometry;
        const auto meshRevision = _resources.GeometryBuffer->GetMeshCacheRevision();
        const bool meshChanged = remote.MeshRevision != meshRevision;
        remote.MeshRevision = meshRevision;
        in.Geometry.VertexResource = mirror(18, meshChanged || !scene.Skin.empty());
        in.Geometry.IndexResource = mirror(19, meshChanged);
        in.Vertex = bind(in.Geometry.VertexResource);
        in.Index = bind(in.Geometry.IndexResource);
        in.Geometry.Vertices.BufferLocation = in.Geometry.VertexResource->GetGPUVirtualAddress();
        in.Geometry.Indices.BufferLocation = in.Geometry.IndexResource->GetGPUVirtualAddress();
        in.Draws = scene.Draws;
        in.Batches = scene.Batches;
        in.Instances = scene.InstanceCount;
        auto& frame = *r.FrameConstants[slot];
        auto main = _resources.Lighting;
        main.RenderTargetSize = {float(_common.DownscaledWidth), float(_common.DownscaledHeight)};
        main.TotalTime = _common.GameTimer->TotalTime();
        main.DeltaTime = _common.GameTimer->DeltaTime();
        main.SunShadowFlags = 0;
        const auto& lights = _resources.LocalLights;
        main.LocalLightCount = static_cast<UINT>(lights.size());
        frame.MainCB->CopyData(0, main);
        remote.Lights->Resize((std::max)(1u, main.LocalLightCount));
        for (UINT i = 0; i < main.LocalLightCount; ++i)
        {
            auto light = lights[i];
            light.ShadowFirstSlice = UINT_MAX;
            light.ShadowFlags = 0;
            remote.Lights->CopyData(i, light);
        }
        in.Main = frame.MainCB->GetElementAddress(0);
        auto* skyBuffer = constants.SkyCB->GetResource().D3DResource.Get();
        auto* sky = mirror(NativeWork::ResourceFor(primary, primaryBindings, skyBuffer, "probe sky constants"), true);
        in.Sky = sky->GetGPUVirtualAddress();
        in.Lights = remote.Lights->GetSRV()->GPUHandle;
        in.StaticShadows = in.DynamicShadows = &remote.ShadowFallback;
        const Id mainResource = bind(frame.MainCB->GetResource().D3DResource.Get());
        for (const auto buffer : {mainResource, bind(remote.Lights->GetResource().D3DResource.Get())})
            in.Reads.push_back({buffer, Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ});
        in.Reads.push_back({bind(sky), Mode::Read, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER});

        const auto textureInput = [&](GDX12Texture& texture) {
            auto* native = texture.GetResource()->D3DResource.Get();
            auto found = std::find(primaryBindings.begin(), primaryBindings.end(), native);
            if (found != primaryBindings.end())
                return Id(found - primaryBindings.begin());
            const auto state = texture.GetResource()->GetCurrentState();
            const Id source = NativeWork::ResourceFor(primary, primaryBindings, native, "probe texture input");
            primary.Resources[source].Initial = primary.Resources[source].Final = state;
            return source;
        };
        const bool environmentChanged = remote.EnvironmentRevision != r.ReflectionEnvironmentRevision;
        remote.EnvironmentRevision = r.ReflectionEnvironmentRevision;
        for (UINT i = 0; i < 4; ++i)
        {
            auto& source = *environment.Output(i)->GetTexture();
            auto* texture = mirror(textureInput(source), environmentChanged);
            if (remote.EnvironmentResources[i].Get() != texture)
            {
                auto& local = remote.Environment[i];
                auto& handle = remote.EnvironmentResources[i];
                handle = ComPtr<ID3D12Resource>(texture);
                GDX12TextureDesc view{};
                view.Width = source.GetWidth();
                view.Height = source.GetHeight();
                view.Format = source.GetFormat();
                view.ExternalResource = handle;
                view.SRV_UAV_Heap = heap;
                view.SRVDesc = source.ShaderViewDescription();
                view.SRVHeapIndex = local.IsInitialized() ? local.GetSRV()->HeapIndex
                                                          : heap->GetAvailableIndex(SceneDescriptorStart);
                local.Initialize(view);
            }
            in.Environment[i] = &remote.Environment[i];
            in.Reads.push_back({bind(texture), Mode::Read, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE});
        }
        if (auto* source = environment.CaptureTransmittance())
        {
            auto* texture = mirror(textureInput(*source), environmentChanged);
            auto view = source->ShaderViewDescription();
            r.SkyTransmittanceSRV.InitAsSRV(texture, &view, heap, r.SkyTransmittanceSRV.HeapIndex);
            in.Reads.push_back({bind(texture), Mode::Read, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE});
        }
        const bool materialsChanged = remote.TextureRevision != _resources.TextureRevision;
        remote.TextureRevision = _resources.TextureRevision;
        std::vector<Access> restoreMaterials;
        for (const auto& [name, texture] : _resources.Textures)
        {
            const UINT index = texture->GetSRV()->HeapIndex;
            if (index < Texture2D_StartIndex || index >= Texture2D_StartIndex + Texture2D_RangeLength) continue;
            const Id source = textureInput(*texture);
            auto* local = mirror(source, materialsChanged);
            if (materialsChanged)
            {
                auto view = texture->ShaderViewDescription();
                r.Device->GetDevice()->CreateShaderResourceView(local, &view, heap->GetCPUHandle(index));
                restoreMaterials.push_back({source, Mode::Read, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE});
            }
            const Id localId = bind(local);
            if (std::none_of(in.Reads.begin(), in.Reads.end(), [=](auto access) { return access.Resource == localId; }))
                in.Reads.push_back({localId, Mode::Read, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE});
        }
        in.Vegetation = [&](const GeometryTarget& target) {
            geometry.AppendVegetationView(primary, primaryBindings, remote.Mirror, graph, bindings, work,
                                          *remote.Vegetation, slot, target, remote.SceneViews[2],
                                          in.Scene[2], mainResource, in.Main, in.Lights);
        };
        std::vector<Id> captureCopies;
        AppendCapture(graph, bindings, work, p, r, in, captureCopies, remote.Filtered);
        remote.RemoteImport = remote.Mirror.AppendImport(graph, bindings, work, true);
        // GPU0 bindless draws sample these textures outside per-texture graph declarations.
        remote.PrimaryExport = remote.Mirror.AppendExport(primary, primaryBindings, _work,
            materialsChanged ? Queue::Direct : Queue::Copy);
        if (!restoreMaterials.empty())
            _work.Add(primary, "restore GPU0 bindless material textures", std::move(restoreMaterials), [](auto*) {},
                      {remote.PrimaryExport});
        remote.WorkPlanned = true;
    }

    const int mode = TransferMode.GetValue();
    const UINT count = UINT(_owners.size());
    for (UINT visited = 0; visited < count; ++visited)
    {
        const UINT index = (_transferProbe + visited) % count;
        const Id id = _owners[index];
        if (!id) continue;
        auto& remote = *_remotes.at(id);
        if (remote.LastWork && !_submissions->Complete(remote.LastWork)) continue;
        auto& generation = remote.Probes.at(index);
        auto& destination = _passes->_probes[index];
        if (!generation.Filtered || generation.Imported || !generation.Captured ||
            !_submissions->Complete(generation.Captured) || !SameReflectionCapture(generation.Source, destination.Source))
            continue;
        const UINT first = generation.Faces;
        const UINT faces = mode == 2 ? 1 : 6 - first;
        const UINT sourceCube = UINT(std::find(remote.Indices.begin(), remote.Indices.end(), index) - remote.Indices.begin());
        if (remote.PrimaryExport == Invalid)
            remote.PrimaryExport = _work.Add(primary, "dispatch remote reflection probe",
                {{scene.FrameIds[0], Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ}}, [](auto*) {});
        generation.Transfer->AppendExport(remote.Plan, remote.Bindings, remote.Work,
            remote.Pass._published.GetResource()->D3DResource.Get(), first, faces, sourceCube);
        const Id imported = generation.Transfer->AppendImport(primary, primaryBindings, _work, first, faces);
        primary.Operations[imported].Dependencies.push_back(remote.PrimaryImports.empty()
            ? remote.PrimaryExport : remote.PrimaryImports.front());
        remote.PrimaryImports.push_back(imported);
        generation.ImportOperation = imported;
        generation.Faces += faces;
        generation.Publish = generation.Faces == 6;
        if (generation.Publish)
        {
            generation.ImportOperation = generation.Transfer->AppendPublication(primary, primaryBindings, _work,
                _passes->_published.GetResource()->D3DResource.Get(), index);
            publications.push_back(generation.ImportOperation);
            destination.Published = generation.Source;
            destination.Ready = true;
            _publishing.push_back(index);
        }
        remote.WorkPlanned = true;
        if (mode != 0)
        {
            _transferProbe = generation.Publish ? (index + 1) % count : index;
            break;
        }
    }
}

void SceneReflections::AddExchanges(const Schedule& primary, std::vector<PacketWait>&,
                                   std::vector<Executor::PacketExchange>& exchanges)
{
    const auto packetFor = [](const Schedule& schedule, Id operation) {
        if (operation == Invalid) return Invalid;
        const auto packet = std::find_if(schedule.Packets.begin(), schedule.Packets.end(),
                                         [=](const Packet& value) { return value.Operation == operation; });
        if (packet == schedule.Packets.end()) throw std::logic_error("Missing reflection probe graph packet");
        return Id(packet - schedule.Packets.begin());
    };
    for (auto& [id, entry] : _remotes)
    {
        auto& remote = *entry;
        if (!remote.WorkPlanned) continue;
        const Id dispatch = packetFor(primary, remote.PrimaryExport);
        const Id firstImport = remote.PrimaryImports.empty() ? Invalid : packetFor(primary, remote.PrimaryImports.front());
        for (auto& [index, generation] : remote.Probes)
            generation.ImportOperation = packetFor(primary, generation.ImportOperation);
        auto schedule = Compile(remote.Plan);
        const Id inputImport = packetFor(schedule, remote.RemoteImport);
        remote.GPU.Begin(remote.Resources.CurrFrameConstantsIndex);
        remote.GPU.Reserve(schedule);
        exchanges.push_back({dispatch, firstImport, [&remote, schedule = std::move(schedule), inputImport](CompletionToken source) {
            const auto record = [&remote](Id op, UINT, ID3D12GraphicsCommandList* cmd) {
                if (!remote.Work.Record(op, cmd)) throw std::logic_error("Missing remote reflection work");
            };
            std::vector<PacketWait> waits;
            if (inputImport != Invalid) waits.push_back({inputImport, source});
            remote.GPU.Execute(schedule, remote.Bindings, record, nullptr, nullptr, nullptr, waits);
            remote.LastWork = remote.GPU.Completions().back();
            for (const UINT slot : remote.Filtered)
            {
                auto& generation = remote.Probes.at(remote.Indices[slot]);
                generation.Source = remote.Pass._probes[slot].Published;
                generation.Captured = remote.LastWork;
                generation.Faces = 0;
                generation.Filtered = true;
                generation.Publish = false;
                ++generation.Number;
            }
            return remote.LastWork;
        }});
    }
}

void SceneReflections::RemoteSubmitted(const std::vector<CompletionToken>& completions)
{
    for (auto& [id, entry] : _remotes)
        for (auto& [index, generation] : entry->Probes)
            if (generation.ImportOperation != Invalid)
            {
                generation.Imported = completions.at(generation.ImportOperation);
                if (generation.Imported && generation.Publish && !generation.Reported)
                    generation.FirstMaterialFrame = completions.back();
                generation.ImportOperation = Invalid;
            }
}

void SceneReflections::AppendCapture(Graph& graph, std::vector<ID3D12Resource*>& bindings, NativeWork& work,
                                     GDX12ReflectionProbePass& p, GDX12DeviceResources& resources,
                                     const CaptureInputs& in, std::vector<Id>& publicationOperations,
                                     std::vector<UINT>& publishing)
{
    const auto resource = [&](GDX12Resource* value) {
        return NativeWork::ResourceFor(graph, bindings, value->D3DResource.Get(), "probe resource");
    };
    const auto texture = [&](GDX12Texture* value, Mode mode, State state) {
        return Access{resource(value->GetResource()), mode, state};
    };
    const State read = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    auto& frame = p._frames.at(resources.CurrFrameConstantsIndex);
    auto* heap = resources.SRV_UAV_Heap->GetHeap().Get();
    const UINT count = in.Draws;
    if (p._capturePlanned)
    {
        if (!frame.Commands || frame.Commands->GetElementCount() < (std::max)(count, 1u))
        {
            frame.Commands = std::make_unique<GDX12UploadBuffer<GDX12IndirectDrawArgs>>(
                resources.Device, (std::max)(count, 1u), EBufferType::Default, false);
            frame.Commands->CreateUAV(resources.SRV_UAV_Heap.get(),
                                      resources.SRV_UAV_Heap->GetAvailableIndex(SceneDescriptorStart));
        }
        if (in.Batches)
        {
            if (!frame.Batches)
                frame.Batches = std::make_unique<GDX12InstanceBatcher>(resources.Device);
            frame.Batches->ReserveRecording((std::max)(count, 1u), in.Batches, in.Instances);
        }
        else
            frame.Batches.reset();
        if (p._sunShadowNeedsClear)
        {
            const auto dsv = p._sunDSV[0]->CPUHandle;
            work.Add(graph, "initialize probe sun-shadow fallback",
                     {texture(&p._sunShadow, Mode::Write, D3D12_RESOURCE_STATE_DEPTH_WRITE)},
                     [dsv](auto* cmd) { cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr); });
            p._sunShadowNeedsClear = false;
        }
    }
    for (auto& view : p._views)
    {
        if (!view.Selected)
            continue;
        p.SelectProbe(view.Probe);
        auto& state = p._probes[view.Probe];
        const bool shadow = view.Context.Stage == GDX12GeometryStage::Shadow;
        const UINT face = view.Face, baseMip = p._publishedMips - p._mips, slice = face * p._publishedMips + baseMip;
        const auto camera = frame.Cameras->GetElementAddress(view.Probe * 6 + face),
                   shadowCB = frame.Shadow->GetElementAddress(0);
        auto* color = view.Bake ? state.BakedColor.get() : state.Raw.get();
        auto* depth = shadow ? &p._sunShadow : (view.Bake ? state.BakedDepth.get() : &p._depth);
        const auto dsv =
            shadow ? p._sunDSV[0]->CPUHandle : (view.Bake ? state.BakedDSVs[face] : p._depthDSVs[slice])->CPUHandle;
        const auto rtv = shadow ? D3D12_CPU_DESCRIPTOR_HANDLE{}
                                : (view.Bake ? state.BakedRTVs[face] : state.RawRTVs[slice])->CPUHandle;
        if (!shadow && !view.Bake && view.Filter == p.DynamicGeometry)
        {
            auto* sourceColor = state.BakedColor->GetResource()->D3DResource.Get();
            auto* sourceDepth = state.BakedDepth->GetResource()->D3DResource.Get();
            auto* targetColor = color->GetResource()->D3DResource.Get();
            auto* targetDepth = depth->GetResource()->D3DResource.Get();
            work.Add(graph, "probe reuse baked static receivers",
                     {texture(state.BakedColor.get(), Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE),
                      texture(state.BakedDepth.get(), Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE),
                      texture(color, Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST),
                      texture(depth, Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST)},
                     [sourceColor, sourceDepth, targetColor, targetDepth, face, slice](auto* cmd) {
                         const CD3DX12_TEXTURE_COPY_LOCATION cs(sourceColor, face), cd(targetColor, slice),
                             ds(sourceDepth, face), dd(targetDepth, slice);
                         cmd->CopyTextureRegion(&cd, 0, 0, 0, &cs, nullptr);
                         cmd->CopyTextureRegion(&dd, 0, 0, 0, &ds, nullptr);
                     });
        }
        auto* cullRoot = p._cullRoot.get();
        const auto commandUav = frame.Commands->GetUAV()->GPUHandle, counterUav = frame.Counter->GetUAV()->GPUHandle;
        auto* clearPSO = p._clearPSO.Get();
        auto* cullPSO = p._cullPSO.Get();
        const Id commands = resource(&frame.Commands->GetResource()), counter = resource(&frame.Counter->GetResource());
        work.Add(graph, "probe clear visible count",
                 {{commands, Mode::Write, UnorderedAccess}, {counter, Mode::Write, UnorderedAccess}}, [=](auto* cmd) {
                     cmd->SetDescriptorHeaps(1, &heap);
                     cmd->SetComputeRootSignature(cullRoot->GetRootSignature().Get());
                     cmd->SetComputeRootDescriptorTable(cullRoot->GetURootParamIndex(0), commandUav);
                     cmd->SetComputeRootDescriptorTable(cullRoot->GetURootParamIndex(1), counterUav);
                     cmd->SetPipelineState(clearPSO);
                     cmd->Dispatch(1, 1, 1);
                 });
        const UINT filter = UINT(view.Filter);
        const std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 3> inputs{
            in.SceneViews[0], in.SceneViews[1], in.SceneViews[2]};
        work.Add(graph, "probe cull real scene",
                 {{in.Scene[0], Mode::Read, read},
                  {in.Scene[1], Mode::Read, read},
                  {in.Scene[2], Mode::Read, read},
                  {commands, Mode::Write, UnorderedAccess},
                  {counter, Mode::ReadWrite, UnorderedAccess},
                  {resource(&frame.Cameras->GetResource()), Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ},
                  {resource(&frame.Shadow->GetResource()), Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ}},
                 [=](auto* cmd) {
                     cmd->SetDescriptorHeaps(1, &heap);
                     cmd->SetComputeRootSignature(cullRoot->GetRootSignature().Get());
                     cmd->SetPipelineState(cullPSO);
                     const UINT parameters[]{filter, shadow ? 1u : 0u};
                     cmd->SetComputeRoot32BitConstants(0, 2, parameters, 0);
                     cmd->SetComputeRootConstantBufferView(cullRoot->GetBRootParamIndex(1), camera);
                     cmd->SetComputeRootConstantBufferView(cullRoot->GetBRootParamIndex(2), shadowCB);
                     for (UINT i = 0; i < 3; ++i)
                         cmd->SetComputeRootDescriptorTable(cullRoot->GetTRootParamIndex(i), inputs[i]);
                     cmd->SetComputeRootDescriptorTable(cullRoot->GetURootParamIndex(0), commandUav);
                     cmd->SetComputeRootDescriptorTable(cullRoot->GetURootParamIndex(1), counterUav);
                     if (count)
                         cmd->Dispatch((count + 63) / 64, 1, 1);
                 });
        ID3D12Resource* drawCommands = frame.Commands->GetResource().D3DResource.Get();
        D3D12_GPU_VIRTUAL_ADDRESS visible = in.Geometry.Vertices.BufferLocation;
        if (frame.Batches)
        {
            auto* batch = frame.Batches.get();
            const auto resources = batch->RecordingResources();
            for (UINT stage = 0; stage < 3; ++stage)
            {
                std::vector<Access> accesses{{in.Scene[0], Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE},
                                             {commands, Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE},
                                             {counter, Mode::ReadWrite, UnorderedAccess}};
                for (auto* buffer : resources)
                    accesses.push_back({NativeWork::ResourceFor(graph, bindings, buffer, "probe skeletal batch"),
                                        Mode::ReadWrite, UnorderedAccess});
                const auto instanceAddress = bindings.at(in.Scene[0])->GetGPUVirtualAddress(),
                           inputAddress = frame.Commands->GetElementAddress(0),
                           counterAddress = frame.Counter->GetElementAddress(0);
                const UINT batches = in.Batches;
                work.Add(graph, "probe skeletal batching " + std::to_string(stage), accesses, [=](auto* cmd) {
                    batch->RecordStage(cmd, stage, (std::max)(count, 1u), batches, instanceAddress, inputAddress,
                                       counterAddress);
                });
            }
            drawCommands = resources[1];
            visible = batch->VisibleInstancesAddress();
        }
        view.Lighting.LocalShadows = view.Filter == p.StaticGeometry ? in.StaticShadows : in.DynamicShadows;
        view.Lighting.Irradiance = in.Environment[0];
        view.Lighting.Prefiltered = in.Environment[1];
        view.Lighting.BRDF = in.Environment[2];
        view.Context.CameraConstants = camera;
        view.Context.CaptureLighting = shadow ? nullptr : &view.Lighting;
        auto reads = in.Reads;
        reads.push_back({resource(&frame.Cameras->GetResource()), Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ});
        reads.push_back({resource(&frame.Shadow->GetResource()), Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ});
        if (!shadow)
        {
            reads.push_back(texture(&p._sunShadow, Mode::Read, read));
            if (view.Lighting.LocalShadows) reads.push_back(texture(view.Lighting.LocalShadows, Mode::Read, read));
        }
        std::vector<Access> draws = reads;
        for (UINT id : {0u, 2u, 3u})
            draws.push_back({in.Scene[id], Mode::Read, read});
        draws.push_back(
            {in.Vertex, Mode::Read,
             D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE});
        draws.push_back({in.Index, Mode::Read, D3D12_RESOURCE_STATE_INDEX_BUFFER});
        draws.push_back({NativeWork::ResourceFor(graph, bindings, drawCommands, "probe draw commands"), Mode::Read,
                         D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT});
        draws.push_back({counter, Mode::Read, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT});
        draws.push_back(texture(depth, Mode::Write, D3D12_RESOURCE_STATE_DEPTH_WRITE));
        if (!shadow)
            draws.push_back(texture(color, Mode::Write, D3D12_RESOURCE_STATE_RENDER_TARGET));
        if (frame.Batches)
            draws.push_back({NativeWork::ResourceFor(graph, bindings, frame.Batches->RecordingResources()[2],
                                                     "probe visible instances"),
                             Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE});
        auto* root = p._captureRoot.get();
        auto* meshPSO = shadow ? p._shadowPSO.Get() : p._capturePSO.Get();
        auto* skyPSO = p._skyPSO.Get();
        auto* signature = p._signature.Get();
        const UINT resolution = shadow ? p._sunShadow.GetWidth() : p._resolution;
        const auto main = in.Main, sky = in.Sky;
        const auto vb = in.Geometry.Vertices;
        const auto ib = in.Geometry.Indices;
        std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 12> srvs{};
        srvs[0] = in.SceneViews[2];
        srvs[1] = in.SceneViews[3];
        srvs[2] = in.SceneViews[0];
        srvs[3] = p._sunShadow.GetSRV()->GPUHandle;
        for (UINT i = 0; i < 3; ++i)
            srvs[4 + i] = in.Environment[i]->GetSRV()->GPUHandle;
        srvs[7] = in.Lights;
        srvs[8] = view.Lighting.LocalShadows->GetSRV()->GPUHandle;
        srvs[9] = in.Environment[3]->GetSRV()->GPUHandle;
        srvs[10] = resources.SkyTransmittanceSRV.GPUHandle;
        srvs[11] = resources.SRV_UAV_Heap->GetGPUHandle(Texture2D_StartIndex);
        auto* countBuffer = frame.Counter->GetResource().D3DResource.Get();
        const bool clear = shadow || view.Filter != p.DynamicGeometry;
        work.Add(graph, shadow ? "probe sun shadow mesh" : ProbeColorOperationName, draws, [=](auto* cmd) {
            cmd->SetDescriptorHeaps(1, &heap);
            cmd->SetGraphicsRootSignature(root->GetRootSignature().Get());
            cmd->SetGraphicsRootConstantBufferView(root->GetBRootParamIndex(1), main);
            cmd->SetGraphicsRootConstantBufferView(root->GetBRootParamIndex(2), camera);
            cmd->SetGraphicsRootConstantBufferView(root->GetBRootParamIndex(3), shadowCB);
            cmd->SetGraphicsRootConstantBufferView(root->GetBRootParamIndex(4), sky);
            for (UINT i = 0; i < 12; ++i)
                if (!shadow || i < 3 || i >= 10)
                    cmd->SetGraphicsRootDescriptorTable(root->GetTRootParamIndex(i), srvs[i]);
            const auto geometryRoot = root->GetGeometryRootParamIndex();
            cmd->SetGraphicsRootShaderResourceView(geometryRoot, vb.BufferLocation);
            cmd->SetGraphicsRootShaderResourceView(geometryRoot + 1, visible);
            const D3D12_VIEWPORT vp{0, 0, float(resolution), float(resolution), 0, 1};
            const D3D12_RECT rect{0, 0, LONG(resolution), LONG(resolution)};
            cmd->RSSetViewports(1, &vp);
            cmd->RSSetScissorRects(1, &rect);
            cmd->OMSetRenderTargets(shadow ? 0 : 1, shadow ? nullptr : &rtv, FALSE, &dsv);
            cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            if (clear)
            {
                cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, shadow ? 1.f : 0.f, 0, 0, nullptr);
                if (!shadow)
                {
                    const float zero[4] = {};
                    cmd->ClearRenderTargetView(rtv, zero, 0, nullptr);
                    cmd->SetPipelineState(skyPSO);
                    cmd->DrawInstanced(3, 1, 0, 0);
                }
            }
            cmd->SetPipelineState(meshPSO);
            cmd->IASetVertexBuffers(0, 1, &vb);
            cmd->IASetIndexBuffer(&ib);
            if (count)
                cmd->ExecuteIndirect(signature, count, drawCommands, 0, countBuffer, 0);
        });
        GeometryTarget target;
        target.View = view.Context;
        target.Depth = resource(depth->GetResource());
        target.Camera = resource(&frame.Cameras->GetResource());
        target.Dsv = dsv;
        target.Width = target.Height = resolution;
        target.AdditionalReads = reads;
        if (!shadow)
        {
            target.Colors = {resource(color->GetResource())};
            target.Rtvs = {rtv};
        }
        in.Vegetation(target);
        if (!shadow)
        {
            if (view.Bake)
            {
                state.BakedFaces |= 1u << face;
                ++p._statistics.BakedFaces;
            }
            else
            {
                state.CapturedFaces |= 1u << face;
                ++p._statistics.CapturedFaces;
            }
        }
    }
    for (UINT probe = 0; probe < p._probes.size(); ++probe)
    {
        auto& state = p._probes[probe];
        if (state.CapturedFaces != 63)
            continue;
        p.SelectProbe(probe);
        const UINT base = p._publishedMips - p._mips, mips = p._mips;
        auto* root = p._filterRoot.get();
        auto* pipeline = p._filterPSO.Get();
        const auto input = state.RawMipSRVs[base]->GPUHandle;
        const UINT samples = state.Source.PrefilterSamples;
        for (UINT mip = 0; mip < mips; ++mip)
        {
            const UINT size = p._resolution >> mip;
            const float roughness = float(mip) / (mips - 1);
            const auto output = p._filterUAVs[base + mip]->GPUHandle;
            work.Add(graph, "probe GGX prefilter mip " + std::to_string(mip),
                     {texture(state.Raw.get(), Mode::Read, read), texture(&p._filtered, Mode::Write, UnorderedAccess)},
                     [=](auto* cmd) {
                         cmd->SetDescriptorHeaps(1, &heap);
                         cmd->SetComputeRootSignature(root->GetRootSignature().Get());
                         cmd->SetPipelineState(pipeline);
                         const struct
                         {
                             UINT Size;
                             float Roughness;
                             UINT Samples;
                         } c{size, roughness, samples};
                         cmd->SetComputeRoot32BitConstants(0, 3, &c, 0);
                         cmd->SetComputeRootDescriptorTable(root->GetTRootParamIndex(0), input);
                         cmd->SetComputeRootDescriptorTable(root->GetURootParamIndex(0), output);
                         cmd->Dispatch((size + 7) / 8, (size + 7) / 8, 6);
                     });
        }
        auto* source = p._filtered.GetResource()->D3DResource.Get();
        auto* destination = p._published.GetResource()->D3DResource.Get();
        const UINT publishedMips = p._publishedMips;
        publicationOperations.push_back(
            work.Add(graph, "probe publication copy",
                     {texture(&p._filtered, Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE),
                      texture(&p._published, Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST)},
                     [=](auto* cmd) {
                         for (UINT face = 0; face < 6; ++face)
                             for (UINT mip = 0; mip < mips; ++mip)
                             {
                                 const CD3DX12_TEXTURE_COPY_LOCATION src(source, face * publishedMips + base + mip),
                                     dst(destination, (probe * 6 + face) * publishedMips + base + mip);
                                 cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                             }
                     }));
        state.CapturedFaces = 0;
        if (state.RebakePending)
        {
            state.BakedFaces = 0;
            state.RebakePending = false;
        }
        state.Ready = true;
        state.Published = state.Source;
        state.LastUpdate = p._frameNumber;
        ++p._statistics.PublishedProbes;
        publishing.push_back(probe);
        if (!p._settings.Scheduled)
            p._roundRobinProbe = (probe + 1) % UINT(p._probes.size());
    }
}
} // namespace BistroRender

#include "SceneReflections.h"
#include "BistroProbeSettings.h"
#include "ProbeInputMirror.h"
#include "ProbeTransfer.h"
#include "ShadowGeometry.h"
#include "Engine.Core/Logging/Logger.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include <cstring>
#include <numeric>
namespace BistroRender
{
namespace
{
AutoConsoleVariable<int> TransferMode(L"Render.ReflectionProbes.TransferMode", 0,
    L"Remote transfer schedule: 0 all ready cubemaps, 1 one cubemap, 2 one face per frame",
    EConsoleVariableFlags::Default, [](int value) { return value >= 0 && value <= 2; });
constexpr UINT AllProbeFaces = (1u << 6) - 1;
}
struct SceneReflections::CaptureInputs
{
    std::array<Id, 4> Scene{};
    std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 4> SceneViews{};
    Id Vertex = Invalid, Index = Invalid;
    GDX12GeometryBuffer::RecordingBinding Geometry{};
    UINT Draws = 0, Batches = 0, Instances = 0;
    D3D12_GPU_VIRTUAL_ADDRESS Main = 0, Sky = 0;
    D3D12_GPU_DESCRIPTOR_HANDLE LocalLights{};
    std::array<GDX12Texture*, 4> Environment{};
    std::vector<Access> Reads;
    std::function<void(const GeometryTarget&)> Vegetation;
};

struct SceneReflections::Remote
{
    struct Generation
    {
        std::unique_ptr<ProbeTransfer> Transfer;
        GDX12ReflectionProbe Source;
        GDX12ResourceHandle Filtered;
        CompletionToken Captured, Imported;
        CompletionToken FirstMaterialFrame;
        Id ImportOperation = Invalid;
        UINT FilteredFaces = 0, PendingFaces = 0, SourceCube = 0;
        std::array<ReflectionStamp, 6> Stamps{};
        uint64_t Number = 0;
        uint64_t StaticGeometryRevision = 0;
        bool Reported = false;
    };

    Remote(const std::shared_ptr<QueueSubmission>& submissions, Id id, UINT slots)
        : Device(id), GPU(submissions, id, slots), Mirror(*submissions, id), SceneVersions(slots)
    {
        Resources.Initialize(&submissions->Device(id));
        Pass.Setup(&Resources, &Common);
        Pass.Initialize();
        Vegetation = std::make_unique<SceneGeometry::RemoteProbeVegetation>(Resources);
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
    std::unique_ptr<GDX12UploadBuffer<GDX12LocalLightConstants>> Lights;
    std::array<GDX12Descriptor, 4> SceneViews;
    std::vector<std::array<uint64_t, 4>> SceneVersions;
    std::array<std::unique_ptr<GDX12Texture>, 4> Environment;
    NativeWork Work;
    Graph Plan;
    std::vector<ID3D12Resource*> Bindings;
    std::map<UINT, Generation> Probes;
    std::vector<UINT> Indices;
    std::vector<FilteredFaces> Filtered;
    std::vector<Id> PrimaryImports;
    CompletionToken LastWork;
    Id PrimaryExport = Invalid, RemoteImport = Invalid;
    uint64_t EnvironmentRevision = UINT64_MAX;
    struct GeometryVersion
    {
        uint64_t Mesh = UINT64_MAX, Deformation = 0;
    } MirroredGeometry, PlannedGeometry;
    bool WorkPlanned = false;
};

SceneReflections::SceneReflections(GDX12DeviceResources& r, RenderPipelineCommonData& c)
    : _resources(r), _common(c), _metadata(r.FrameConstants.size())
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
        _sceneBudget = {};
        _frame.Probes.clear();
        _due.clear();
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
    for (const auto& publication : _publishing)
    {
        auto& readiness = _publications.at(publication.Probe);
        // Incremental refreshes must not move a still-pending bootstrap fence forward.
        if (!readiness.Fence && !readiness.Ready) readiness.Fence = fence;
    }
    if (_metadataChanges.empty()) return;
    auto& uploaded = _metadata.at(_metadataSlot);
    const auto* data = _resources.FrameConstants[_metadataSlot]->ReflectionProbesUpload->GetCPUData();
    for (const auto& [first, end] : _metadataChanges)
        for (UINT i = first; i < end; ++i)
        {
            uploaded.Initialized[i] = true;
            uploaded.Data[i] = data[i];
        }
    _metadataChanges.clear();
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
    for (size_t i = 0; i < _publications.size(); ++i)
    {
        const auto& publication = _publications[i];
        const auto& state = p._probes[i];
        ready += state.Ready && SameReflectionCapture(state.Source, state.Published) &&
            (publication.Ready || (publication.Fence && publication.Fence <= done));
    }
    return {ready, p._probes.size()};
}

void SceneReflections::AppendPublishedOwners(std::vector<std::pair<std::string, Id>>& owners) const
{
    if (!_passes->_enabled) return;
    for (size_t i = 0; i < _owners.size() && i < _passes->_probes.size(); ++i)
    {
        const auto& probe = _passes->_probes[i];
        if (probe.Ready && SameReflectionCapture(probe.Source, probe.Published))
            owners.emplace_back("Probe " + std::to_string(probe.Source.BistroIndex == UINT_MAX ? i : probe.Source.BistroIndex), _owners[i]);
    }
}
bool SceneReflections::CanCapture(size_t probe) const
{
    const auto owner = _owners[probe];
    if (!owner) return true;
    const auto found = _remotes.find(owner);
    if (found == _remotes.end()) return false;
    const auto& remote = *found->second;
    if (remote.LastWork && !_submissions->Complete(remote.LastWork)) return false;
    const auto& generation = remote.Probes.at(UINT(probe));
    return !generation.Filtered || (!generation.PendingFaces && generation.Imported &&
        _submissions->Complete(generation.Imported) &&
        (!generation.FirstMaterialFrame || _submissions->Complete(generation.FirstMaterialFrame)));
}

void SceneReflections::StampCapturedFaces(UINT probe, UINT mask, std::array<ReflectionStamp, 6>& stamps)
{
    for (UINT face = 0; face < 6; ++face)
        if (mask & (1u << face))
        {
            const auto due = std::exchange(_due[probe][face], 0);
            stamps[face] = {due ? due : _stamp.Version, _stamp.Captured};
        }
}

void SceneReflections::PrepareCaptureBudget()
{
    const auto& probes = _resources.ReflectionProbes;
    _frame.Probes.assign(_owners.size(), {});
    _captureMasks.assign(_owners.size(), 0);
    std::vector<size_t> order(_owners.size());
    std::iota(order.begin(), order.end(), size_t{0});
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return probes[a].BistroIndex < probes[b].BistroIndex; });
    const auto [ready, expected] = Readiness();
    _bootstrapping = ready != _owners.size() || expected != _owners.size();
    std::vector<uint64_t> identities;
    for (const auto i : order)
    {
        const auto& source = probes[i];
        _frame.Probes[i].Identity = source.Id;
        if (!_bootstrapping)
        {
            auto resolved = source;
            static_cast<GDX12ReflectionCaptureSettings&>(resolved) = ResolveReflectionCaptureSettings(source, _settings.Quality);
            _bootstrapping = !SameReflectionCapture(_passes->_probes[i].Published, resolved);
        }
        identities.push_back(source.Id);
    }
    using Frequency = GDX12ReflectionSchedule::Frequency;
    const auto frequency = _bootstrapping || !_useSceneBudget ? Frequency::Off :
        GDX12ReflectionSchedule::CanonicalFrequency(_settings.Frequency, identities.size());
    if (_budgetFrequency != frequency || _due.size() != _owners.size() || _bootstrapping)
        _due.assign(_owners.size(), {});
    _budgetFrequency = frequency;
    const auto masks = _sceneBudget.Select(identities, frequency);
    size_t budget = 0;
    struct Request { uint64_t Due; size_t Probe; UINT Mask; };
    std::vector<Request> requests;
    for (size_t rank = 0; rank < order.size(); ++rank)
    {
        const auto probe = order[rank];
        if (probes[probe].Implementation == 2) continue;
        _frame.Probes[probe].DueFaces = masks[rank];
        budget += masks[rank] != 0;
        auto& due = _due[probe];
        for (UINT face = 0; face < 6; ++face)
            if (!due[face] && (masks[rank] & (1u << face))) due[face] = _stamp.Version;
        if (!CanCapture(probe)) continue;
        for (UINT face = 0; face < 6; ++face)
            if (due[face])
            {
                requests.push_back({due[face], probe, frequency == Frequency::Low ? 1u << face : AllProbeFaces});
                if (frequency != Frequency::Low) break;
            }
    }
    // A busy owner keeps its oldest work. The budget is still shared by every owner.
    std::stable_sort(requests.begin(), requests.end(), [](const auto& a, const auto& b) { return a.Due < b.Due; });
    for (size_t i = 0; i < (std::min)(budget, requests.size()); ++i)
        _captureMasks[requests[i].Probe] = requests[i].Mask;
}

void SceneReflections::Append(Graph& graph, std::vector<ID3D12Resource*>& bindings, SceneGeometry& geometry,
                              SceneEnvironment& environment, UINT slot)
{
    auto& p = *_passes;
    auto& work = _work;
    work.Commands.clear();
    _publishing.clear();
    auto& scene = geometry._slots.at(slot);
    p._externalSettings = &_settings;
    ++_frame.Version;
    _stamp = {_frame.Version, std::chrono::steady_clock::now()};
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
                p._probes[i].CapturedFaces = p._probes[i].RawFaces = p._probes[i].BakedFaces = p._probes[i].NextFace = 0;
                p._probes[i].LastUpdate = 0;
            }
        _owners = std::move(owners);
        _transferProbe = 0;
    }
    using Frequency = GDX12ReflectionSchedule::Frequency;
    const bool legacyFaceBudget = !BistroReflections::UseQualityPreset.GetValue() && _settings.Frequency == Frequency::Epic &&
        std::any_of(_resources.ReflectionProbes.begin(), _resources.ReflectionProbes.end(),
            [](const auto& probe) { return probe.FacesPerFrame && probe.FacesPerFrame < 6; });
    _useSceneBudget = !_settings.Scheduled && !legacyFaceBudget;
    PrepareCaptureBudget();
    p._sceneFaces = _useSceneBudget ? std::function<UINT(UINT)>([this](UINT i) { return _captureMasks[i]; }) : nullptr;
    p._ownsCapture = [this](UINT index) { return _owners.at(index) == 0; };
    p.SetInputs({environment.Output(0), environment.Output(1), environment.Output(2), environment.Output(3)});
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
        if (!p._probes[i].Ready || !SameReflectionCapture(p._probes[i].Source, p._probes[i].Published))
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
        in.LocalLights = constants.LocalLightCache->GetSRV()->GPUHandle;
        for (UINT i = 0; i < 4; ++i) in.Environment[i] = environment.Output(i)->GetTexture();
        in.Reads = environment.Reads(graph, bindings);
        for (auto* input : {&constants.MainCB->GetResource(), &constants.SkyCB->GetResource(),
                            &constants.LocalLightCache->GetResource()})
            in.Reads.push_back({resource(input), Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ});
        in.Vegetation = [&](const GeometryTarget& target) { geometry.AppendVegetationView(graph, bindings, slot, target); };
        AppendCapture(graph, bindings, work, p, _resources, in, publicationOperations, _publishing);
        for (const auto [probe, mask] : _publishing)
            StampCapturedFaces(probe, mask, publications[probe].Faces);
    }
    AppendRemote(graph, bindings, geometry, environment, slot, publicationOperations, replaced);
    const UINT count = p._enabled ? UINT(p._probes.size()) : 1;
    auto* source = constants.ReflectionProbesUpload->GetResource().D3DResource.Get();
    auto* target = constants.ReflectionProbes->GetResource().D3DResource.Get();
    auto& uploaded = _metadata.at(slot);
    if (uploaded.Target != target) uploaded = {target};
    _metadataSlot = slot;
    auto& changed = _metadataChanges;
    changed.clear();
    for (UINT i = 0; i < count; ++i)
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
            _frame.Probes[i].Used = publications[i].Faces;
        }
        if (uploaded.Initialized[i] && std::memcmp(&uploaded.Data[i], &data, sizeof(data)) == 0) continue;
        constants.ReflectionProbesUpload->CopyData(i, data);
        if (!changed.empty() && changed.back().second == i) ++changed.back().second;
        else changed.emplace_back(i, i + 1);
    }
    if (uploaded.Count != count)
    {
        auto* metadata = constants.ReflectionProbes->GetSRV();
        D3D12_SHADER_RESOURCE_VIEW_DESC probeView{};
        probeView.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        probeView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        probeView.Buffer.StructureByteStride = sizeof(GDX12ReflectionProbeData);
        probeView.Buffer.NumElements = count;
        metadata->InitAsSRV(target, &probeView, _resources.SRV_UAV_Heap.get(), metadata->HeapIndex);
        uploaded.Count = count;
    }
    if (!changed.empty())
        work.Add(
            graph, "probe metadata upload (GPU ordered)",
            {{resource(&constants.ReflectionProbesUpload->GetResource()), Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ},
             {resource(&constants.ReflectionProbes->GetResource()), Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST}},
            [source, target, changed](auto* cmd) {
                constexpr UINT64 stride = sizeof(GDX12ReflectionProbeData);
                for (const auto& [first, end] : changed)
                    cmd->CopyBufferRegion(target, first * stride, source, first * stride, (end - first) * stride);
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
    if (_remotes.empty()) return;

    std::vector<GDX12Texture*> materialTextures;
    const auto live = geometry.LiveProbeCaptureInputs(environment, slot, &materialTextures);

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
        remote.Mirror.RetireUnused(live, [&](ID3D12Resource* local) {
            remote.Vegetation->Forget(local);
            for (auto& texture : remote.Environment)
                if (texture && texture->GetResource()->D3DResource.Get() == local) texture.reset();
        });
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
                if (!generation.PendingFaces) generation.Filtered.Reset();
            }
            const bool staticChanged = source.Implementation == 2 &&
                generation.StaticGeometryRevision != _resources.ReflectionStaticGeometryRevision;
            if ((!SameReflectionCapture(generation.Source, source) || staticChanged) && !generation.Imported)
            {
                generation.Filtered.Reset();
                generation.FilteredFaces = generation.PendingFaces = 0;
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
        remote.Common.CameraView = _common.CameraView;
        p._externalSettings = &_settings;
        p._sceneFaces = _useSceneBudget ? std::function<UINT(UINT)>([this, &remote](UINT i) {
            return _captureMasks[remote.Indices[i]];
        }) : nullptr;
        p._frameNumber = _passes->_frameNumber - 1;
        p._captureAllowed = [&](UINT index) {
            const auto& generation = remote.Probes.at(remote.Indices[index]);
            return !generation.Filtered && !generation.Imported;
        };
        p.SetInputs({environment.Output(0), environment.Output(1), environment.Output(2), environment.Output(3)});
        p.Prepare();
        p._planPrepared = false;
        if (!p._capturePlanned) continue;

        auto* heap = r.SRV_UAV_Heap.get();
        CaptureInputs in;
        const auto mirror = [&](Id source, bool refresh) {
            return remote.Mirror.Mirror(primary, primaryBindings, primaryBindings.at(source), source, refresh);
        };
        const auto bind = [&](ID3D12Resource* resource) {
            return NativeWork::ResourceFor(graph, bindings, resource, "remote probe input");
        };
        constexpr UINT strides[]{sizeof(GDX12InstanceData), sizeof(GDX12IndirectDrawArgs),
                                 sizeof(GDX12MaterialConstants), sizeof(GDX12TransformConstants)};
        for (UINT i = 0; i < 4; ++i)
        {
            const auto& buffer = scene.Buffers[i];
            auto* native = primaryBindings.at(i);
            auto& previous = remote.SceneVersions.at(slot)[i];
            std::vector<ProbeInputMirror::BufferRange> ranges;
            if (previous + 1 == buffer.ContentRevision && buffer.Upload && remote.Mirror.Contains(native))
                for (const auto& [first, end] : buffer.Ranges)
                    ranges.push_back({UINT64(first) * strides[i], UINT64(end - first) * strides[i]});
            const bool refresh = previous != buffer.ContentRevision && ranges.empty();
            auto* local = remote.Mirror.Mirror(primary, primaryBindings, native, i, refresh, {}, std::move(ranges));
            previous = buffer.ContentRevision;
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
        const bool meshChanged = remote.MirroredGeometry.Mesh != meshRevision;
        auto* sourceVertices = primaryBindings.at(SceneVertices);
        const bool seedVertices = meshChanged || !remote.Mirror.Contains(sourceVertices);
        std::vector<ProbeInputMirror::BufferRange> vertexRanges;
        if (!seedVertices && remote.MirroredGeometry.Deformation != geometry._vertexDeformationRevision)
            for (const auto& [first, end] : geometry.ChangedVertexRanges(remote.MirroredGeometry.Deformation))
                vertexRanges.push_back({UINT64(first) * sizeof(Vertex), UINT64(end - first) * sizeof(Vertex)});
        in.Geometry.VertexResource = remote.Mirror.Mirror(primary, primaryBindings, sourceVertices,
            SceneVertices, seedVertices, {}, std::move(vertexRanges));
        in.Geometry.IndexResource = mirror(SceneIndices, meshChanged);
        remote.PlannedGeometry = {meshRevision, geometry._vertexDeformationRevision};
        in.Vertex = bind(in.Geometry.VertexResource);
        in.Index = bind(in.Geometry.IndexResource);
        in.Geometry.Vertices.BufferLocation = in.Geometry.VertexResource->GetGPUVirtualAddress();
        in.Geometry.Indices.BufferLocation = in.Geometry.IndexResource->GetGPUVirtualAddress();
        in.Draws = scene.Draws;
        in.Batches = scene.Batches;
        in.Instances = scene.InstanceCount;
        auto& main = *r.FrameConstants.at(slot)->MainCB;
        main.CopyData(0, constants.MainCB->GetCPUData()[0]);
        in.Main = main.GetElementAddress(0);
        auto* skyBuffer = constants.SkyCB->GetResource().D3DResource.Get();
        auto* sky = mirror(NativeWork::ResourceFor(primary, primaryBindings, skyBuffer, "probe sky constants"), true);
        in.Sky = sky->GetGPUVirtualAddress();
        const Id mainResource = bind(main.GetResource().D3DResource.Get());
        in.Reads.push_back({mainResource, Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ});
        in.Reads.push_back({bind(sky), Mode::Read, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER});
        const auto& lights = _resources.LocalLights;
        const UINT lightCount = (std::max)(1u, UINT(lights.size()));
        if (!remote.Lights)
        {
            remote.Lights = std::make_unique<GDX12UploadBuffer<GDX12LocalLightConstants>>(r.Device, lightCount, EBufferType::Upload, false);
            remote.Lights->CreateSRV(heap, heap->GetAvailableIndex(SceneDescriptorStart));
        }
        remote.Lights->Resize(lightCount);
        if (!lights.empty()) remote.Lights->CopyData(0, lights.data(), UINT(lights.size()));
        in.LocalLights = remote.Lights->GetSRV()->GPUHandle;
        in.Reads.push_back({bind(remote.Lights->GetResource().D3DResource.Get()), Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ});

        const auto textureInput = [&](GDX12Texture& texture) {
            auto* native = texture.GetResource()->D3DResource.Get();
            const Id existing = primary.FindBinding(primaryBindings, native);
            if (existing != Invalid) return existing;
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
            auto& local = remote.Environment[i];
            if (!local || local->GetResource()->D3DResource.Get() != texture)
            {
                if (!local) local = std::make_unique<GDX12Texture>();
                GDX12TextureDesc view{};
                view.Width = source.GetWidth();
                view.Height = source.GetHeight();
                view.Format = source.GetFormat();
                view.ExternalResource = ComPtr<ID3D12Resource>(texture);
                view.SRV_UAV_Heap = heap;
                view.SRVDesc = source.ShaderViewDescription();
                view.SRVHeapIndex = local->IsInitialized() ? local->GetSRV()->HeapIndex
                                                           : heap->GetAvailableIndex(SceneDescriptorStart);
                local->Initialize(view);
            }
            in.Environment[i] = local.get();
            in.Reads.push_back({bind(texture), Mode::Read, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE});
        }
        if (auto* source = environment.CaptureTransmittance())
        {
            auto* texture = mirror(textureInput(*source), environmentChanged);
            auto view = source->ShaderViewDescription();
            r.SkyTransmittanceSRV.InitAsSRV(texture, &view, heap, r.SkyTransmittanceSRV.HeapIndex);
            in.Reads.push_back({bind(texture), Mode::Read, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE});
        }
        std::vector<Access> restoreMaterials;
        for (auto* texture : materialTextures)
        {
            const UINT index = texture->GetSRV()->HeapIndex;
            const Id source = textureInput(*texture);
            auto* native = primaryBindings[source];
            const bool canvas = std::find(scene.ProbeCanvases.begin(), scene.ProbeCanvases.end(), texture) !=
                scene.ProbeCanvases.end();
            auto* local = mirror(source, canvas);
            auto view = texture->ShaderViewDescription();
            r.Device->GetDevice()->CreateShaderResourceView(local, &view, heap->GetCPUHandle(index));
            if (remote.Mirror.PendingCopy(native) && std::none_of(restoreMaterials.begin(), restoreMaterials.end(),
                                                                  [=](auto access) { return access.Resource == source; }))
                restoreMaterials.push_back({source, Mode::Read, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE});
            const Id localId = bind(local);
            if (std::none_of(in.Reads.begin(), in.Reads.end(), [=](auto access) { return access.Resource == localId; }))
                in.Reads.push_back({localId, Mode::Read, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE});
        }
        in.Vegetation = [&](const GeometryTarget& target) {
            geometry.AppendVegetationView(primary, primaryBindings, remote.Mirror, graph, bindings, work,
                                          *remote.Vegetation, slot, target, remote.SceneViews[2],
                                          in.Scene[2], mainResource, in.Main,
                                          target.View.CaptureLighting ? target.View.CaptureLighting->LocalLightSrv :
                                                                        D3D12_GPU_DESCRIPTOR_HANDLE{});
        };
        std::vector<Id> captureCopies;
        AppendCapture(graph, bindings, work, p, r, in, captureCopies, remote.Filtered);
        for (const auto [probe, mask] : remote.Filtered)
        {
            const auto index = remote.Indices[probe];
            StampCapturedFaces(index, mask, remote.Probes.at(index).Stamps);
        }
        remote.RemoteImport = remote.Mirror.AppendImport(graph, bindings, work, true);
        // GPU0 bindless draws sample these textures outside per-texture graph declarations.
        remote.PrimaryExport = remote.Mirror.AppendExport(primary, primaryBindings, _work,
            restoreMaterials.empty() ? Queue::Copy : Queue::Direct);
        if (!restoreMaterials.empty())
            _work.Add(primary, "restore GPU0 bindless material textures", std::move(restoreMaterials), [](auto*) {},
                      {remote.PrimaryExport});
        remote.WorkPlanned = true;
    }

    const int mode = _useSceneBudget && (_bootstrapping || _settings.Frequency <= GDX12ReflectionSchedule::Frequency::Low)
        ? 0 : TransferMode.GetValue();
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
        if (!generation.Filtered || !generation.PendingFaces || generation.Imported || !generation.Captured ||
            !_submissions->Complete(generation.Captured) || !SameReflectionCapture(generation.Source, destination.Source))
            continue;
        if (remote.PrimaryExport == Invalid)
            remote.PrimaryExport = _work.Add(primary, "dispatch remote reflection probe",
                {{scene.FrameIds[0], Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ}}, [](auto*) {});
        auto* published = _passes->_published.GetResource()->D3DResource.Get();
        const UINT pending = generation.PendingFaces;
        for (UINT first = 0; first < 6; ++first)
        {
            if (!(generation.PendingFaces & (1u << first))) continue;
            UINT faces = 1;
            if (mode != 2)
                while (first + faces < 6 && (generation.PendingFaces & (1u << (first + faces)))) ++faces;
            generation.Transfer->AppendExport(remote.Plan, remote.Bindings, remote.Work,
                generation.Filtered.Get(), first, faces, generation.SourceCube);
            const Id imported = generation.Transfer->AppendImport(primary, primaryBindings, _work, published, first, faces, index);
            primary.Operations[imported].Dependencies.push_back(remote.PrimaryImports.empty()
                ? remote.PrimaryExport : remote.PrimaryImports.front());
            remote.PrimaryImports.push_back(imported);
            generation.ImportOperation = imported;
            generation.PendingFaces &= ~(((1u << faces) - 1) << first);
            if (mode == 2) break;
            first += faces - 1;
        }
        const bool finished = generation.PendingFaces == 0;
        const bool compatible = destination.Ready && SameReflectionCapture(destination.Published, generation.Source);
        if (!compatible) _publications[index] = {};
        if (compatible || (finished && generation.FilteredFaces == AllProbeFaces))
        {
            publications.push_back(generation.ImportOperation);
            destination.Published = generation.Source;
            destination.Ready = true;
            const UINT mask = compatible ? pending & ~generation.PendingFaces : AllProbeFaces;
            _publishing.push_back({index, mask});
            for (UINT face = 0; face < 6; ++face)
                if (mask & (1u << face)) _publications[index].Faces[face] = generation.Stamps[face];
        }
        else
        {
            // The first six faces fill the array in place, but metadata must hide the partial cube.
            destination.Ready = false;
        }
        remote.WorkPlanned = true;
        if (mode != 0)
        {
            _transferProbe = finished ? (index + 1) % count : index;
            break;
        }
    }
}

void SceneReflections::AddExchanges(const Schedule& primary, std::vector<PacketWait>&,
                                   std::vector<Executor::PacketExchange>& exchanges)
{
    for (auto& [id, entry] : _remotes)
    {
        auto& remote = *entry;
        if (!remote.WorkPlanned) continue;
        const Id dispatch = primary.PacketFor(remote.PrimaryExport);
        const Id firstImport = remote.PrimaryImports.empty() ? Invalid : primary.PacketFor(remote.PrimaryImports.front());
        for (auto& [index, generation] : remote.Probes)
            generation.ImportOperation = primary.OptionalPacketFor(generation.ImportOperation);
        auto schedule = Compile(remote.Plan);
        const Id inputImport = schedule.OptionalPacketFor(remote.RemoteImport);
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
            if (inputImport != Invalid) remote.MirroredGeometry = remote.PlannedGeometry;
            for (const auto& filtered : remote.Filtered)
            {
                auto& generation = remote.Probes.at(remote.Indices[filtered.Probe]);
                generation.Source = remote.Pass._probes[filtered.Probe].Published;
                generation.Filtered = remote.Pass._published.GetResource()->D3DResource;
                generation.SourceCube = filtered.Probe;
                generation.Captured = remote.LastWork;
                generation.FilteredFaces = generation.PendingFaces = filtered.Mask;
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
                if (generation.Imported && !generation.PendingFaces &&
                    generation.FilteredFaces == AllProbeFaces && !generation.Reported)
                    generation.FirstMaterialFrame = completions.back();
                generation.ImportOperation = Invalid;
            }
}

void SceneReflections::AppendCapture(Graph& graph, std::vector<ID3D12Resource*>& bindings, NativeWork& work,
                                     GDX12ReflectionProbePass& p, GDX12DeviceResources& resources,
                                     const CaptureInputs& in, std::vector<Id>& publicationOperations,
                                     std::vector<FilteredFaces>& publishing)
{
    const auto resource = [&](GDX12Resource* value) {
        return NativeWork::ResourceFor(graph, bindings, value->D3DResource.Get(), "probe resource");
    };
    const auto texture = [&](GDX12Texture* value, Mode mode, State state) {
        return Access{resource(value->GetResource()), mode, state};
    };
    const State read = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    auto& frame = p._frames.at(resources.CurrFrameConstantsIndex);
    auto& capture = p._capture.value();
    const UINT captureMips = GDX12RenderHelpers::MipCount(p._captureTextureResolution);
    auto* heap = resources.SRV_UAV_Heap->GetHeap().Get();
    const UINT count = in.Draws;
    std::array<UINT, MaxReflectionProbes> capturedNow{};
    std::array<bool, MaxReflectionProbes> needsFullFilter{};
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
    }
    for (auto& view : p._views)
    {
        if (!view.Selected)
            continue;
        p.SelectProbe(view.Probe);
        auto& state = p._probes[view.Probe];
        const UINT face = view.Face, baseMip = captureMips - p._mips, slice = face * captureMips + baseMip;
        const auto camera = frame.Cameras->GetElementAddress(view.Probe * 6 + face);
        auto* depth = view.Bake ? state.BakedDepth.get() : &capture.Depth;
        auto* color = view.Bake ? state.BakedColor.get() : state.Raw.get();
        const auto dsv = (view.Bake ? state.BakedDSVs[face] : p._depthDSVs[slice])->CPUHandle;
        const auto rtv = (view.Bake ? state.BakedRTVs[face] : state.RawRTVs[slice])->CPUHandle;
        if (!view.Bake && view.Filter == p.DynamicGeometry)
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
                  {resource(&frame.Cameras->GetResource()), Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ}},
                 [=](auto* cmd) {
                     cmd->SetDescriptorHeaps(1, &heap);
                     cmd->SetComputeRootSignature(cullRoot->GetRootSignature().Get());
                     cmd->SetPipelineState(cullPSO);
                     cmd->SetComputeRoot32BitConstant(0, filter, 0);
                     cmd->SetComputeRootConstantBufferView(cullRoot->GetBRootParamIndex(1), camera);
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
        view.Lighting.LocalLightSrv = in.LocalLights;
        view.Lighting.Irradiance = in.Environment[0];
        view.Lighting.Prefiltered = in.Environment[1];
        view.Lighting.BRDF = in.Environment[2];
        view.Context.CameraConstants = camera;
        view.Context.CaptureLighting = &view.Lighting;
        auto reads = in.Reads;
        reads.push_back({resource(&frame.Cameras->GetResource()), Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ});
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
        draws.push_back(texture(color, Mode::Write, D3D12_RESOURCE_STATE_RENDER_TARGET));
        if (frame.Batches)
            draws.push_back({NativeWork::ResourceFor(graph, bindings, frame.Batches->RecordingResources()[2],
                                                     "probe visible instances"),
                             Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE});
        auto* root = p._captureRoot.get();
        auto* meshPSO = p._capturePSO.Get();
        auto* skyPSO = p._skyPSO.Get();
        auto* signature = p._signature.Get();
        const UINT resolution = p._resolution;
        const auto main = in.Main, sky = in.Sky;
        const auto vb = in.Geometry.Vertices;
        const auto ib = in.Geometry.Indices;
        std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 12> srvs{};
        srvs[0] = in.SceneViews[2];
        srvs[1] = in.SceneViews[3];
        srvs[2] = in.SceneViews[0];
        for (UINT i = 0; i < 3; ++i)
            srvs[4 + i] = in.Environment[i]->GetSRV()->GPUHandle;
        srvs[7] = view.Lighting.LocalLightSrv;
        srvs[9] = in.Environment[3]->GetSRV()->GPUHandle;
        srvs[10] = resources.SkyTransmittanceSRV.GPUHandle;
        srvs[11] = resources.SRV_UAV_Heap->GetGPUHandle(Texture2D_StartIndex);
        auto* countBuffer = frame.Counter->GetResource().D3DResource.Get();
        const bool clear = view.Filter != p.DynamicGeometry;
        work.Add(graph, ProbeColorOperationName, draws, [=](auto* cmd) {
            cmd->SetDescriptorHeaps(1, &heap);
            cmd->SetGraphicsRootSignature(root->GetRootSignature().Get());
            cmd->SetGraphicsRootConstantBufferView(root->GetBRootParamIndex(1), main);
            cmd->SetGraphicsRootConstantBufferView(root->GetBRootParamIndex(2), camera);
            cmd->SetGraphicsRootConstantBufferView(root->GetBRootParamIndex(4), sky);
            for (UINT i = 0; i < 12; ++i)
                if (srvs[i].ptr)
                    cmd->SetGraphicsRootDescriptorTable(root->GetTRootParamIndex(i), srvs[i]);
            const auto geometryRoot = root->GetGeometryRootParamIndex();
            cmd->SetGraphicsRootShaderResourceView(geometryRoot, vb.BufferLocation);
            cmd->SetGraphicsRootShaderResourceView(geometryRoot + 1, visible);
            const D3D12_VIEWPORT vp{0, 0, float(resolution), float(resolution), 0, 1};
            const D3D12_RECT rect{0, 0, LONG(resolution), LONG(resolution)};
            cmd->RSSetViewports(1, &vp);
            cmd->RSSetScissorRects(1, &rect);
            cmd->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
            cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            if (clear)
            {
                cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.f, 0, 0, nullptr);
                const float zero[4] = {};
                cmd->ClearRenderTargetView(rtv, zero, 0, nullptr);
                cmd->SetPipelineState(skyPSO);
                cmd->DrawInstanced(3, 1, 0, 0);
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
        target.Colors = {resource(color->GetResource())};
        target.Rtvs = {rtv};
        in.Vegetation(target);
        if (view.Bake)
        {
            state.BakedFaces |= 1u << face;
            ++p._statistics.BakedFaces;
        }
        else
        {
            if (!capturedNow[view.Probe])
                needsFullFilter[view.Probe] = state.RawFaces != AllProbeFaces || !state.Ready;
            capturedNow[view.Probe] |= 1u << face;
            state.RawFaces |= 1u << face;
            state.CapturedFaces |= 1u << face;
            ++p._statistics.CapturedFaces;
        }
    }
    for (UINT probe = 0; probe < p._probes.size(); ++probe)
    {
        auto& state = p._probes[probe];
        if (!capturedNow[probe] || state.RawFaces != AllProbeFaces)
            continue;
        const UINT filtered = needsFullFilter[probe] ? AllProbeFaces : capturedNow[probe];
        UINT faceCount = 0;
        for (UINT face = 0; face < 6; ++face)
            if (filtered & (1u << face)) ++faceCount;
        p.SelectProbe(probe);
        const UINT base = captureMips - p._mips, mips = p._mips;
        auto* root = p._filterRoot.get();
        const UINT samples = state.Source.PrefilterSamples;
        auto* scratch = capture.Filtered.GetResource()->D3DResource.Get();
        auto* raw = state.Raw->GetResource()->D3DResource.Get();
        const auto dispatchFilter = [&](UINT mip, UINT sourceMip, float roughness, ID3D12PipelineState* pipeline,
                                        const char* name) {
            const UINT size = p._resolution >> mip;
            const auto input = state.RawMipSRVs[base + sourceMip]->GPUHandle;
            const auto output = p._filterUAVs[base + mip]->GPUHandle;
            work.Add(graph, name + std::to_string(mip),
                     {texture(state.Raw.get(), Mode::Read, read), texture(&capture.Filtered, Mode::Write, UnorderedAccess)},
                     [=](auto* cmd) {
                         cmd->SetDescriptorHeaps(1, &heap);
                         cmd->SetComputeRootSignature(root->GetRootSignature().Get());
                         cmd->SetPipelineState(pipeline);
                         const struct
                         {
                             UINT Size;
                             float Roughness;
                             UINT Samples;
                             UINT Faces;
                         } c{size, roughness, samples, filtered};
                         cmd->SetComputeRoot32BitConstants(0, 4, &c, 0);
                         cmd->SetComputeRootDescriptorTable(root->GetTRootParamIndex(0), input);
                         cmd->SetComputeRootDescriptorTable(root->GetURootParamIndex(0), output);
                         cmd->Dispatch((size + 7) / 8, (size + 7) / 8, faceCount);
                     });
        };
        // Retain other faces' raw pyramids between incremental captures. On first publication
        // all six base faces are complete and all six pyramids are built before GGX samples them.
        for (UINT mip = 1; mip < mips; ++mip)
        {
            dispatchFilter(mip, mip - 1, 0, p._downsamplePSO.Get(), "probe radiance downsample mip ");
            work.Add(graph, "store probe radiance mip " + std::to_string(mip),
                     {texture(&capture.Filtered, Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE),
                      texture(state.Raw.get(), Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST)},
                     [=](auto* cmd) {
                         for (UINT face = 0; face < 6; ++face)
                         {
                             if (!(filtered & (1u << face))) continue;
                             const UINT slice = face * captureMips + base + mip;
                             const CD3DX12_TEXTURE_COPY_LOCATION src(scratch, slice), dst(raw, slice);
                             cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                         }
                     });
        }
        for (UINT mip = 0; mip < mips; ++mip)
            dispatchFilter(mip, 0, float(mip) / (mips - 1), p._filterPSO.Get(), "probe GGX prefilter mip ");
        auto* source = scratch;
        auto* destination = p._published.GetResource()->D3DResource.Get();
        const UINT publishedMips = p._publishedMips;
        const UINT publishedBaseMip = publishedMips - mips;
        publicationOperations.push_back(
            work.Add(graph, "probe publication copy",
                     {texture(&capture.Filtered, Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE),
                      texture(&p._published, Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST)},
                     [=](auto* cmd) {
                         for (UINT face = 0; face < 6; ++face)
                         {
                             if (!(filtered & (1u << face))) continue;
                             for (UINT mip = 0; mip < mips; ++mip)
                             {
                                 const CD3DX12_TEXTURE_COPY_LOCATION src(source, face * captureMips + base + mip),
                                     dst(destination, (probe * 6 + face) * publishedMips + publishedBaseMip + mip);
                                 cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                             }
                         }
                     }));
        state.Ready = true;
        state.Published = state.Source;
        ++p._statistics.PublishedProbes;
        publishing.push_back({probe, filtered});
        // Sweep completion controls scheduling only. Once bootstrapped, each captured face publishes immediately.
        if (state.CapturedFaces == AllProbeFaces)
        {
            state.CapturedFaces = 0;
            if (state.RebakePending)
            {
                state.BakedFaces = 0;
                state.RebakePending = false;
            }
            state.LastUpdate = p._frameNumber;
            if (!p._settings.Scheduled)
                p._roundRobinProbe = (probe + 1) % UINT(p._probes.size());
        }
    }
}
} // namespace BistroRender

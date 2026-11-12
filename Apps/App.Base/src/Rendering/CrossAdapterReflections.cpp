#include "App.Base/Rendering/CrossAdapterReflections.h"
#include "Engine.RendererDX12/GDX12TextureResource.h"
#include <algorithm>
#include <stdexcept>

namespace
{
using Graph = GDX12RenderGraph;
using namespace GDX12Submission;
constexpr auto ShaderRead = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
}

CrossAdapterReflections::CrossAdapterReflections(GDX12DeviceResources& primary, GDX12Device& secondary,
    RenderMGPURuntime& runtime, MaterialTextures materials, RenderPipelineCommonData& common,
    GDX12ReflectionProbePass& pass, GDX12EnvironmentIBLPass& environment)
    : _primary(primary), _secondaryDevice(secondary), _runtime(runtime), _materials(std::move(materials)), _common(common),
      _pass(pass), _environment(environment), _work(runtime)
{ _pass.EnableSecondaryCapture(true); }
CrossAdapterReflections::~CrossAdapterReflections() = default;

bool CrossAdapterReflections::HasSecondaryWork() const
{
    const auto& work = _pass.ScheduledWork();
    return std::any_of(work.begin(), work.end(), [](const auto& item) { return item.SecondaryFaces != 0; });
}

void CrossAdapterReflections::ResetWorld()
{
    if (_capture) _capture->ResetHistory();
    _textures.clear();
    _sceneBytes = _resultBytes = 0;
    _work.WaitMilliseconds = 0;
}

GDX12Texture* CrossAdapterReflections::MirrorTexture(GDX12Texture* source, bool material, bool refresh)
{
    if (!source || !source->IsInitialized()) return nullptr;
    auto& mirror = _textures[source];
    const bool changed = mirror.Source != source->GetResourceOwner();
    if (changed)
    {
        mirror.Texture.reset();
        mirror.Source = source->GetResourceOwner();
        auto native = mirror.Source->D3DResource->GetDesc();
        native.Flags = D3D12_RESOURCE_FLAG_NONE;
        GDX12TextureDesc desc;
        desc.Width = source->GetWidth(); desc.Height = source->GetHeight(); desc.Format = source->GetFormat();
        desc.SRV_UAV_Heap = _secondary->SRV_UAV_Heap.get();
        desc.SRVDesc = source->ShaderViewDescription();
        desc.SRVHeapIndex = material ? source->GetSRV()->HeapIndex :
            desc.SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
        desc.ExternalResource = _secondaryDevice.CreateResource(D3D12_HEAP_TYPE_DEFAULT, native, D3D12_RESOURCE_STATE_COMMON);
        mirror.Texture = std::make_unique<GDX12Texture>();
        mirror.Texture->Initialize(desc);
    }
    if (changed || refresh)
        _sceneBytes += _work.Copy(_sceneChannel, *_primary.Device, _secondaryDevice,
            {{_work.Identify(mirror.Source, *_primary.Device), _work.Identify(mirror.Texture->GetResourceOwner(), _secondaryDevice)}});
    return mirror.Texture.get();
}

void CrossAdapterReflections::PrepareScene()
{
    if (!_secondary)
    {
        _secondary = std::make_unique<GDX12DeviceResources>();
        _secondary->Initialize(&_secondaryDevice);
        _secondary->CurrFrameConstantsIndex = 0;
        _capture = std::make_unique<GDX12ReflectionProbePass>();
        _capture->Setup(_secondary.get(), nullptr, &_common);
        _capture->Initialize();
        _capture->SetSplitGeometry(true);
    }
    auto& scene = *_secondary;
    scene.Lighting = _primary.Lighting;
    scene.Sky = _primary.Sky;
    scene.ShadowSettings = _primary.ShadowSettings;
    scene.ShadowQuality = _primary.ShadowQuality;
    scene.LocalLights = _primary.LocalLights;
    scene.LocalShadowCasters = _primary.LocalShadowCasters;
    scene.StaticMeshShadowCasters = _primary.StaticMeshShadowCasters;
    scene.StaticMeshShadowRevision = _primary.StaticMeshShadowRevision;
    scene.LocalShadowVisibility = _primary.LocalShadowVisibility;
    scene.ReflectionProbes = _primary.ReflectionProbes;
    scene.ReflectionStaticRevision = _primary.ReflectionStaticRevision;
    scene.ReflectionDynamicRevision = _primary.ReflectionDynamicRevision;
    scene.ReflectionEnvironmentRevision = _primary.ReflectionEnvironmentRevision;
    scene.ReflectionSceneBounds = _primary.ReflectionSceneBounds;
    scene.GeometryRevision = _primary.GeometryRevision;
    scene.SkeletalBatchCount = _primary.SkeletalBatchCount;
    scene.SkeletalInstanceCount = _primary.SkeletalInstanceCount;
    auto& source = *_primary.FrameConstants[_primary.CurrFrameConstantsIndex];
    auto& target = *scene.FrameConstants[0];
    target.MainCB->CopyData(0, scene.Lighting);
    source.SkyCB->TrackChanges();
    target.SkyCB->CopyData(0, source.SkyCB->GetCPUData()[0]);
    target.DisplayShadowPolicy = source.DisplayShadowPolicy;
    std::vector<CrossAdapterWork::CopyRegion> copies;
    const auto buffer = [&](const auto& from, auto& to)
    {
        using Buffer = typename std::decay_t<decltype(to)>::element_type;
        const auto count = (std::max)(1u, from->GetElementCount());
        D3D12_HEAP_PROPERTIES heap{};
        if (to && to->GetResource().D3DResource) ThrowIfFailed(to->GetResource().D3DResource->GetHeapProperties(&heap, nullptr));
        if (!to || to->GetElementCount() != count || heap.Type != D3D12_HEAP_TYPE_DEFAULT)
        {
            to = std::make_unique<Buffer>(&_secondaryDevice, count, EBufferType::Default, false);
            to->CreateSRV(scene.SRV_UAV_Heap.get(), scene.SRV_UAV_Heap->GetAvailableIndex(ConstantsResources));
        }
        if (from->GetElementCount()) copies.push_back({_work.Identify(from->ShareResource(), *_primary.Device),
            _work.Identify(to->ShareResource(), _secondaryDevice)});
    };
    buffer(source.ShadowTemplates, target.ShadowTemplates);
    buffer(source.ShadowInstances, target.ShadowInstances);
    buffer(source.ShadowTransforms, target.ShadowTransforms);
    buffer(source.ShadowMaterials, target.ShadowMaterials);
    buffer(source.ShadowLights, target.ShadowLights);
    if (source.DisplayShadowPolicy) buffer(source.DisplayShadowLights, target.DisplayShadowLights);
    const auto geometry = [&](std::shared_ptr<GDX12Resource> from, View& to)
    {
        const auto bytes = from->D3DResource->GetDesc().Width;
        if (!to.Resource || to.Resource->D3DResource->GetDesc().Width != bytes) to = _work.Allocate(_secondaryDevice, bytes);
        copies.push_back({_work.Identify(std::move(from), *_primary.Device), to});
    };
    geometry(_primary.GeometryBuffer->ShareVertexResource(), _vertices);
    geometry(_primary.GeometryBuffer->ShareIndexResource(), _indices);
    scene.GeometryBuffer->SetSnapshotBuffers(_vertices.Resource, _indices.Resource);
    scene.IndirectCommandsCache->Resize(_primary.IndirectCommandsCache->GetElementCount());
    if (_primary.IndirectCommandsCache->GetElementCount())
        scene.IndirectCommandsCache->CopyData(0, _primary.IndirectCommandsCache->GetCPUData(), _primary.IndirectCommandsCache->GetElementCount());
    _sceneBytes += _work.Copy(_sceneChannel, *_primary.Device, _secondaryDevice, copies);
    for (auto* texture : _materials())
    {
        const auto flags = texture->GetResource()->D3DResource->GetDesc().Flags;
        MirrorTexture(texture, true, (flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) != 0);
    }
    std::vector<IRenderPassLink*> inputs;
    for (UINT index = 0; index < 4; ++index) inputs.push_back(MirrorTexture(_environment.GetOutput(index)->GetTexture(), false, true));
    inputs.push_back(MirrorTexture(_pass.CaptureLocalShadows(), false, true));
    _capture->SetInputs(inputs);
    if (auto* transmittance = MirrorTexture(_environment.CaptureTransmittance(), false, true))
    {
        auto view = transmittance->ShaderViewDescription();
        scene.SkyTransmittanceSRV.InitAsSRV(transmittance->GetResource()->D3DResource.Get(), &view,
            scene.SRV_UAV_Heap.get(), scene.SkyTransmittanceSRV.HeapIndex);
    }
    auto work = _pass.ScheduledWork();
    for (auto& item : work) { item.Faces = item.SecondaryFaces; item.SecondaryFaces = 0; }
    _capture->SetRemoteWork(std::move(work), _pass.FrameSourceTime());
    _capture->Prepare();
    _applicationReads = _prepareApplication ? _prepareApplication(scene, *_capture, _work, _sceneBytes) : std::vector<CrossAdapterWork::Use>{};
}

std::vector<CrossAdapterWork::Use> CrossAdapterReflections::PrimaryUses()
{
    std::vector<CrossAdapterWork::Use> uses;
    const auto add = [&](std::shared_ptr<GDX12Resource> resource)
    {
        if (!resource || !resource->D3DResource || std::any_of(uses.begin(), uses.end(),
            [&](const auto& use) { return use.Resource.Resource == resource; })) return;
        const auto state = resource->GetCurrentState() == D3D12_RESOURCE_STATE_GENERIC_READ ?
            D3D12_RESOURCE_STATE_GENERIC_READ : ShaderRead;
        uses.push_back({_work.Identify(std::move(resource), *_primary.Device), Graph::Mode::Read, state});
    };
    const auto& frame = _primary.FrameConstants[_primary.CurrFrameConstantsIndex];
    add(frame->ShadowTemplates->ShareResource()); add(frame->ShadowInstances->ShareResource());
    add(frame->ShadowTransforms->ShareResource()); add(frame->ShadowMaterials->ShareResource());
    add(frame->ShadowLights->ShareResource()); add(frame->LightingLocalLights().ShareResource());
    add(frame->MainCB->ShareResource()); add(frame->SkyCB->ShareResource());
    add(_primary.GeometryBuffer->ShareVertexResource()); add(_primary.GeometryBuffer->ShareIndexResource());
    for (auto& use : uses)
    {
        if (use.Resource.Resource == _primary.GeometryBuffer->ShareVertexResource())
            use.State = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        if (use.Resource.Resource == _primary.GeometryBuffer->ShareIndexResource()) use.State = D3D12_RESOURCE_STATE_INDEX_BUFFER;
    }
    for (UINT index = 0; index < 4; ++index) add(_environment.GetOutput(index)->GetTexture()->GetResourceOwner());
    add(_pass.CaptureLocalShadows()->GetResourceOwner());
    if (auto* transmittance = _environment.CaptureTransmittance()) add(transmittance->GetResourceOwner());
    for (auto* texture : _materials()) add(texture->GetResourceOwner());
    if (_applicationInputs) for (const auto& resource : _applicationInputs()) add(resource);
    const auto writable = [&](std::shared_ptr<GDX12Resource> resource)
    {
        add(resource);
        for (auto& use : uses) if (use.Resource.Resource == resource)
        {
            use.Mode = Graph::Mode::ReadWrite;
            use.State = D3D12_RESOURCE_STATE_COPY_DEST;
        }
    };
    for (UINT probe = 0; probe < _pass.ScheduledWork().size(); ++probe)
        writable(_pass.RawFace(probe, 0).Texture->GetResourceOwner());
    writable(_pass.PublishedTexture().GetResourceOwner());
    writable(frame->ReflectionProbes->ShareResource());
    add(frame->ReflectionProbesUpload->ShareResource());
    return uses;
}

std::optional<std::vector<Graph::LegacyExternalView>> CrossAdapterReflections::Boundary()
{
    if (!HasSecondaryWork()) return std::nullopt;
    std::vector<Graph::LegacyExternalView> views;
    for (const auto& use : PrimaryUses())
    {
        bool linked = use.Resource.Resource == _pass.CaptureLocalShadows()->GetResourceOwner();
        for (UINT index = 0; index < 4; ++index)
            linked |= use.Resource.Resource == _environment.GetOutput(index)->GetTexture()->GetResourceOwner();
        for (auto* input : _materialInputs)
            linked |= use.Resource.Resource == input->GetTexture()->GetResourceOwner();
        if (!linked) views.push_back({use.Resource.Identity, use.Resource.Resource, use.Mode, use.State,
            use.Mode == Graph::Mode::Read ? std::nullopt : std::optional{D3D12_RESOURCE_STATE_COMMON}});
    }
    return views;
}

void CrossAdapterReflections::Configure(Graph& graph, GDX12RenderPass& node, RenderPipelineBindings& bindings,
    std::vector<IRenderPassLink*> materialInputs)
{
    _materialInputs = std::move(materialInputs);
    std::vector<IRenderPassLink*> inputs;
    for (UINT index = 0; index < 4; ++index) inputs.push_back(_environment.GetOutput(index));
    inputs.push_back(_pass.CaptureLocalShadows());
    inputs.insert(inputs.end(), _materialInputs.begin(), _materialInputs.end());
    graph.SetOptionalLegacySplit(node, std::move(inputs), {}, [this] { return Boundary(); });
    bindings.LegacyReplacements.push_back({&node, [this, &node](const auto&, auto prefix)
        -> std::optional<CompletionPoint> { return Submit(node, prefix); },
        [this](CompletionPoint point) { _pass.OnSubmitted(point); }, {}});
}

CompletionPoint CrossAdapterReflections::Submit(GDX12RenderPass& node, CompletionPoint prefix)
{
    _work.Wait(prefix);
    PrepareScene();
    std::vector<CrossAdapterWork::Use> secondaryReads;
    const auto read = [&](std::shared_ptr<GDX12Resource> resource, D3D12_RESOURCE_STATES state = ShaderRead)
    { secondaryReads.push_back({_work.Identify(std::move(resource), _secondaryDevice), Graph::Mode::Read, state}); };
    const auto& frame = _secondary->FrameConstants[0];
    read(frame->ShadowTemplates->ShareResource()); read(frame->ShadowInstances->ShareResource());
    read(frame->ShadowTransforms->ShareResource()); read(frame->ShadowMaterials->ShareResource());
    read(frame->ShadowLights->ShareResource());
    if (frame->DisplayShadowPolicy) read(frame->DisplayShadowLights->ShareResource());
    read(_vertices.Resource, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    read(_indices.Resource, D3D12_RESOURCE_STATE_INDEX_BUFFER);
    for (const auto& item : _textures) read(item.second.Texture->GetResourceOwner());
    secondaryReads.insert(secondaryReads.end(), _applicationReads.begin(), _applicationReads.end());
    const auto captured = _work.Run("Reflection secondary capture and filter", _secondaryDevice, std::move(secondaryReads), [&](GDX12CommandList& commands)
    {
        _capture->BeginGeometry(&commands);
        for (UINT slot = 0; slot < _capture->GetGeometryViewCapacity(); ++slot)
            if (_capture->GetGeometryView(slot))
            {
                _capture->ExecuteGeometryView(&commands, slot);
                if (_drawApplication) _drawApplication(commands, *_capture, slot);
            }
        _capture->EndGeometry(&commands);
        for (UINT probe = 0; probe < _pass.ScheduledWork().size(); ++probe)
            commands.ResourceBarrier({_capture->RawFace(probe, 0).Texture->GetResource()->GetTransitionBarrier(D3D12_RESOURCE_STATE_COMMON)});
        commands.ResourceBarrier({_capture->PublishedTexture().GetResource()->GetTransitionBarrier(D3D12_RESOURCE_STATE_COMMON)});
    });
    _capture->OnSubmitted(captured);
    std::vector<CrossAdapterWork::CopyRegion> copies;
    for (UINT probe = 0; probe < _pass.ScheduledWork().size(); ++probe)
        for (UINT face = 0; face < 6; ++face)
            if (_pass.ScheduledWork()[probe].SecondaryFaces & (1u << face))
            {
                const auto source = _capture->RawFace(probe, face), destination = _pass.RawFace(probe, face);
                copies.push_back({_work.Identify(source.Texture->GetResourceOwner(), _secondaryDevice),
                    _work.Identify(destination.Texture->GetResourceOwner(), *_primary.Device), source.Subresource, destination.Subresource});
            }
    _resultBytes += _work.Copy(_resultChannel, _secondaryDevice, *_primary.Device, copies);
    for (UINT probe = 0; probe < _pass.ScheduledWork().size(); ++probe)
        for (UINT face = 0; face < 6; ++face)
            if (_pass.ScheduledWork()[probe].SecondaryFaces & (1u << face))
                _pass.ImportFace(probe, face, _capture->RawFace(probe, face).SourceTime);
    for (UINT probe = 0; probe < _pass.ScheduledWork().size(); ++probe)
    {
        if (!_capture->PublishedThisFrame(probe) || !_pass.CanImportPublished(probe, _capture->PublishedSourceTimes(probe))) continue;
        copies.clear();
        const auto source = _work.Identify(_capture->PublishedTexture().GetResourceOwner(), _secondaryDevice);
        const auto destination = _work.Identify(_pass.PublishedTexture().GetResourceOwner(), *_primary.Device);
        for (UINT face = 0; face < 6; ++face)
            for (UINT mip = 0; mip < _pass.PublishedMips(); ++mip)
            {
                const UINT sub = (probe * 6 + face) * _pass.PublishedMips() + mip;
                copies.push_back({source, destination, sub, sub});
            }
        _resultBytes += _work.Copy(_resultChannel, _secondaryDevice, *_primary.Device, copies);
        _pass.ImportPublished(probe, _capture->PublishedSourceTimes(probe));
    }
    const auto published = _work.Run("Reflection primary faces and publication", *_primary.Device, PrimaryUses(),
        [&](GDX12CommandList& commands) { node.Execute(&commands); });
    _pass.OnSubmitted(published);
    return published;
}

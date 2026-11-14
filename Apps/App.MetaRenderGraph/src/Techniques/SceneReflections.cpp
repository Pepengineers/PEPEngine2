#include "SceneReflections.h"
#include <cstring>
#include <fstream>
namespace Meta
{
namespace
{
UINT MipCount(UINT size)
{
    UINT count = 1;
    while (size > 1)
    {
        size >>= 1;
        ++count;
    }
    return count;
}
} // namespace
SceneReflections::SceneReflections(GDX12DeviceResources &r, RenderPipelineCommonData &c) : _resources(r), _common(c)
{
    for (UINT mode = 0; mode < 2; ++mode)
    {
        _evidence[mode].resize(r.FrameConstants.size());
        auto &p = _passes[mode];
        p = std::make_unique<GDX12ReflectionProbePass>();
        p->Setup(&r, &c);
        p->Initialize();
        p->CreatePublishedTexture(1, 1);
        _black[mode].D3DResource = r.Device->CreateResource(D3D12_HEAP_TYPE_UPLOAD, CD3DX12_RESOURCE_DESC::Buffer(256),
                                                            D3D12_RESOURCE_STATE_GENERIC_READ);
        void *data = nullptr;
        const D3D12_RANGE noRead{};
        ThrowIfFailed(_black[mode].D3DResource->Map(0, &noRead, &data));
        std::memset(data, 0, 256);
        static_cast<uint16_t *>(data)[3] = 0x3c00;
        _black[mode].D3DResource->Unmap(0, nullptr);
    }
}
SceneReflections::~SceneReflections()
{
    for (auto &p : _passes)
        p->ClearDenendencies();
}
void SceneReflections::Reset()
{
    for (UINT mode = 0; mode < 2; ++mode)
    {
        _passes[mode]->ResetHistory();
        _publications[mode].clear();
    }
}
bool SceneReflections::Record(Id id, ID3D12GraphicsCommandList *cmd) const
{
    return _work[_mode].Record(id, cmd);
}
void SceneReflections::Submitted(bool serial, uint64_t fence)
{
    if (!fence)
        throw std::invalid_argument("Probe publication requires a submitted fence");
    const UINT mode = serial ? 0 : 1;
    for (auto probe : _publishing[mode])
        _publications[mode].at(probe).Fence = fence;
}
std::pair<size_t, size_t> SceneReflections::Readiness() const
{
    const auto &p = *_passes[1];
    if (!p._enabled)
        return {};
    const auto done = _resources.Device->GetCommandQueue()->GetFence()->GetCompletedValue();
    if (done == UINT64_MAX)
        throw std::runtime_error("Probe completion fence reports device removal");
    size_t ready = 0;
    for (const auto &publication : _publications[1])
        ready += publication.Ready || (publication.Fence && publication.Fence <= done);
    return {ready, p._probes.size()};
}
void SceneReflections::Append(Graph &graph, std::vector<ID3D12Resource *> &bindings, SceneGeometry &geometry,
                              SceneEnvironment &environment, UINT slot, bool serial)
{
    _mode = serial ? 0 : 1;
    auto &p = *_passes[_mode];
    auto &work = _work[_mode];
    work.Commands.clear();
    _publishing[_mode].clear();
    auto &scene = geometry._slots.at(slot);
    p._externalSettings = &_settings;
    p._externalStaticShadows = geometry._shadows->LightingLocal(slot, true);
    p._externalDynamicShadows = geometry._shadows->LightingLocal(slot);
    p.SetInputs({environment.Output(0), environment.Output(1), environment.Output(2), environment.Output(3),
                 geometry._shadows->LightingLocal(slot)});
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
    _history[_mode] = _common.HistoryRevision;
    _common.HistoryRevision = history + (serial ? 0 : (_history[_mode] - _historyBase));
    if (serial)
    {
        _common.ResetHistory = reset;
        _common.ResetUpscalerHistory = upscalerReset;
        _common.RequestHistoryReset = requestReset;
    }
    if (_radiance[_mode].HeapIndex == UINT_MAX || replaced)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.TextureCubeArray.NumCubes = p._published.GetResource()->D3DResource->GetDesc().DepthOrArraySize / 6;
        view.TextureCubeArray.MipLevels = p._publishedMips;
        auto &descriptor = _radiance[_mode];
        descriptor.InitAsSRV(p._published.GetResource()->D3DResource.Get(), &view, _resources.SRV_UAV_Heap.get(),
                             descriptor.HeapIndex == UINT_MAX ? _resources.SRV_UAV_Heap->GetAvailableIndex(105000)
                                                              : descriptor.HeapIndex);
    }
    std::vector<Id> publicationOperations;
    auto &publications = _publications[_mode];
    const auto done = _resources.Device->GetCommandQueue()->GetFence()->GetCompletedValue();
    if (replaced)
        publications.assign(p._probes.size(), {});
    else
        publications.resize(p._probes.size());
    for (UINT i = 0; i < publications.size(); ++i)
    {
        auto &publication = publications[i];
        if (!p._probes[i].Ready)
            publication = {};
        else
            publication.Ready |= publication.Fence && publication.Fence <= done;
    }
    const auto resource = [&](GDX12Resource *value) {
        return NativeWork::ResourceFor(graph, bindings, value->D3DResource.Get(), "probe resource");
    };
    const auto texture = [&](GDX12Texture *value, Mode mode, State state) {
        return Access{resource(value->GetResource()), mode, state};
    };
    const State read = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    if (!p._enabled && p._publishedNeedsTransition && p._publishedResolution == 1)
    {
        auto *source = _black[_mode].D3DResource.Get();
        auto *target = p._published.GetResource()->D3DResource.Get();
        work.Add(graph, "initialize disabled probe fallback",
                 {{resource(&_black[_mode]), Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ},
                  texture(&p._published, Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST)},
                 [source, target](auto *cmd) {
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
    auto &frame = p._frames.at(slot);
    auto &constants = *_resources.FrameConstants[slot];
    auto *heap = _resources.SRV_UAV_Heap->GetHeap().Get();
    const UINT count = scene.Draws;
    if (p._capturePlanned)
    {
        if (!frame.Commands || frame.Commands->GetElementCount() < (std::max)(count, 1u))
        {
            frame.Commands = std::make_unique<GDX12UploadBuffer<GDX12IndirectDrawArgs>>(
                _resources.Device, (std::max)(count, 1u), EBufferType::Default, false);
            frame.Commands->CreateUAV(_resources.SRV_UAV_Heap.get(),
                                      _resources.SRV_UAV_Heap->GetAvailableIndex(105000));
        }
        if (scene.Batches)
        {
            if (!frame.Batches)
                frame.Batches = std::make_unique<GDX12InstanceBatcher>(_resources.Device);
            frame.Batches->ReserveRecording((std::max)(count, 1u), scene.Batches, scene.InstanceCount);
        }
        else
            frame.Batches.reset();
        if (p._sunShadowNeedsClear)
        {
            const auto dsv = p._sunDSV[0]->CPUHandle;
            work.Add(graph, "initialize probe sun-shadow fallback",
                     {texture(&p._sunShadow, Mode::Write, D3D12_RESOURCE_STATE_DEPTH_WRITE)},
                     [dsv](auto *cmd) { cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr); });
            p._sunShadowNeedsClear = false;
        }
    }
    for (auto &view : p._views)
    {
        if (!view.Selected)
            continue;
        p.SelectProbe(view.Probe);
        auto &state = p._probes[view.Probe];
        const bool shadow = view.Context.Stage == GDX12GeometryStage::Shadow;
        const UINT face = view.Face, baseMip = p._publishedMips - p._mips, slice = face * p._publishedMips + baseMip;
        const auto camera = frame.Cameras->GetElementAddress(view.Probe * 6 + face),
                   shadowCB = frame.Shadow->GetElementAddress(0);
        auto *color = view.Bake ? state.BakedColor.get() : state.Raw.get();
        auto *depth = shadow ? &p._sunShadow : (view.Bake ? state.BakedDepth.get() : &p._depth);
        const auto dsv =
            shadow ? p._sunDSV[0]->CPUHandle : (view.Bake ? state.BakedDSVs[face] : p._depthDSVs[slice])->CPUHandle;
        const auto rtv = shadow ? D3D12_CPU_DESCRIPTOR_HANDLE{}
                                : (view.Bake ? state.BakedRTVs[face] : state.RawRTVs[slice])->CPUHandle;
        if (!shadow && !view.Bake && view.Filter == p.DynamicGeometry)
        {
            auto *sourceColor = state.BakedColor->GetResource()->D3DResource.Get();
            auto *sourceDepth = state.BakedDepth->GetResource()->D3DResource.Get();
            auto *targetColor = color->GetResource()->D3DResource.Get();
            auto *targetDepth = depth->GetResource()->D3DResource.Get();
            work.Add(graph, "probe reuse baked static receivers",
                     {texture(state.BakedColor.get(), Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE),
                      texture(state.BakedDepth.get(), Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE),
                      texture(color, Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST),
                      texture(depth, Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST)},
                     [sourceColor, sourceDepth, targetColor, targetDepth, face, slice](auto *cmd) {
                         const CD3DX12_TEXTURE_COPY_LOCATION cs(sourceColor, face), cd(targetColor, slice),
                             ds(sourceDepth, face), dd(targetDepth, slice);
                         cmd->CopyTextureRegion(&cd, 0, 0, 0, &cs, nullptr);
                         cmd->CopyTextureRegion(&dd, 0, 0, 0, &ds, nullptr);
                     });
        }
        auto *cullRoot = p._cullRoot.get();
        const auto commandUav = frame.Commands->GetUAV()->GPUHandle, counterUav = frame.Counter->GetUAV()->GPUHandle;
        auto *clearPSO = p._clearPSO.Get();
        auto *cullPSO = p._cullPSO.Get();
        const Id commands = resource(&frame.Commands->GetResource()), counter = resource(&frame.Counter->GetResource());
        work.Add(graph, "probe clear visible count",
                 {{commands, Mode::Write, UnorderedAccess}, {counter, Mode::Write, UnorderedAccess}}, [=](auto *cmd) {
                     cmd->SetDescriptorHeaps(1, &heap);
                     cmd->SetComputeRootSignature(cullRoot->GetRootSignature().Get());
                     cmd->SetComputeRootDescriptorTable(cullRoot->GetURootParamIndex(0), commandUav);
                     cmd->SetComputeRootDescriptorTable(cullRoot->GetURootParamIndex(1), counterUav);
                     cmd->SetPipelineState(clearPSO);
                     cmd->Dispatch(1, 1, 1);
                 });
        const UINT filter = UINT(view.Filter);
        const std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 3> inputs{
            scene.Buffers[0].Srv.GPUHandle, scene.Buffers[1].Srv.GPUHandle, scene.Buffers[2].Srv.GPUHandle};
        work.Add(graph, "probe cull real scene",
                 {{0, Mode::Read, read},
                  {1, Mode::Read, read},
                  {2, Mode::Read, read},
                  {commands, Mode::Write, UnorderedAccess},
                  {counter, Mode::ReadWrite, UnorderedAccess},
                  {resource(&frame.Cameras->GetResource()), Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ},
                  {resource(&frame.Shadow->GetResource()), Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ}},
                 [=](auto *cmd) {
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
        ID3D12Resource *drawCommands = frame.Commands->GetResource().D3DResource.Get();
        D3D12_GPU_VIRTUAL_ADDRESS visible = scene.Geometry.Vertices.BufferLocation;
        if (frame.Batches)
        {
            auto *batch = frame.Batches.get();
            const auto resources = batch->RecordingResources();
            for (UINT stage = 0; stage < 3; ++stage)
            {
                std::vector<Access> accesses{{0, Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE},
                                             {commands, Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE},
                                             {counter, Mode::ReadWrite, UnorderedAccess}};
                for (auto *buffer : resources)
                    accesses.push_back({NativeWork::ResourceFor(graph, bindings, buffer, "probe skeletal batch"),
                                        Mode::ReadWrite, UnorderedAccess});
                const auto instanceAddress = scene.Buffers[0].Data->GetGPUVirtualAddress(),
                           inputAddress = frame.Commands->GetElementAddress(0),
                           counterAddress = frame.Counter->GetElementAddress(0);
                const UINT batches = scene.Batches;
                work.Add(graph, "probe skeletal batching " + std::to_string(stage), accesses, [=](auto *cmd) {
                    batch->RecordStage(cmd, stage, (std::max)(count, 1u), batches, instanceAddress, inputAddress,
                                       counterAddress);
                });
            }
            drawCommands = resources[1];
            visible = batch->VisibleInstancesAddress();
        }
        view.Lighting.LocalShadows = geometry._shadows->LightingLocal(slot, view.Filter == p.StaticGeometry);
        view.Context.CameraConstants = camera;
        view.Context.CaptureLighting = shadow ? nullptr : &view.Lighting;
        auto reads = environment.Reads(graph, bindings);
        reads.push_back({scene.FrameIds[0], Mode::Read, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER});
        reads.push_back({resource(&frame.Cameras->GetResource()), Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ});
        reads.push_back({resource(&frame.Shadow->GetResource()), Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ});
        reads.push_back({resource(&constants.SkyCB->GetResource()), Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ});
        if (!shadow)
        {
            reads.push_back(texture(&p._sunShadow, Mode::Read, read));
            reads.push_back(texture(view.Lighting.LocalShadows, Mode::Read, read));
            reads.push_back({resource(&constants.ShadowLights->GetResource()), Mode::Read, read});
        }
        std::vector<Access> draws = reads;
        for (UINT id : {0u, 2u, 3u})
            draws.push_back({id, Mode::Read, read});
        draws.push_back(
            {21, Mode::Read,
             D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE});
        draws.push_back({22, Mode::Read, D3D12_RESOURCE_STATE_INDEX_BUFFER});
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
        auto *root = p._captureRoot.get();
        auto *meshPSO = shadow ? p._shadowPSO.Get() : p._capturePSO.Get();
        auto *skyPSO = p._skyPSO.Get();
        auto *signature = p._signature.Get();
        const UINT resolution = shadow ? p._sunShadow.GetWidth() : p._resolution;
        const auto main = scene.Main, sky = constants.SkyCB->GetElementAddress(0);
        const auto vb = scene.Geometry.Vertices;
        const auto ib = scene.Geometry.Indices;
        std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 12> srvs{};
        srvs[0] = scene.Buffers[2].Srv.GPUHandle;
        srvs[1] = scene.Buffers[3].Srv.GPUHandle;
        srvs[2] = scene.Buffers[0].Srv.GPUHandle;
        srvs[3] = p._sunShadow.GetSRV()->GPUHandle;
        for (UINT i = 0; i < 3; ++i)
            srvs[4 + i] = environment.Output(i)->GetTexture()->GetSRV()->GPUHandle;
        srvs[7] = constants.ShadowLights->GetSRV()->GPUHandle;
        srvs[8] = view.Lighting.LocalShadows->GetSRV()->GPUHandle;
        srvs[9] = environment.Output(3)->GetTexture()->GetSRV()->GPUHandle;
        srvs[10] = _resources.SkyTransmittanceSRV.GPUHandle;
        srvs[11] = _resources.SRV_UAV_Heap->GetGPUHandle(Texture2D_StartIndex);
        auto *countBuffer = frame.Counter->GetResource().D3DResource.Get();
        const bool clear = shadow || view.Filter != p.DynamicGeometry;
        work.Add(graph, shadow ? "probe sun shadow mesh" : "probe HDR capture mesh/sky", draws, [=](auto *cmd) {
            cmd->SetDescriptorHeaps(1, &heap);
            cmd->SetGraphicsRootSignature(root->GetRootSignature().Get());
            cmd->SetGraphicsRootConstantBufferView(root->GetBRootParamIndex(1), main);
            cmd->SetGraphicsRootConstantBufferView(root->GetBRootParamIndex(2), camera);
            cmd->SetGraphicsRootConstantBufferView(root->GetBRootParamIndex(3), shadowCB);
            cmd->SetGraphicsRootConstantBufferView(root->GetBRootParamIndex(4), sky);
            for (UINT i = 0; i < 12; ++i)
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
        bindings = geometry._grass->Append(graph, slot, bindings, scene.Buffers[2].Srv, {target}, true);
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
        auto &state = p._probes[probe];
        if (state.CapturedFaces != 63)
            continue;
        p.SelectProbe(probe);
        const UINT base = p._publishedMips - p._mips, mips = p._mips;
        auto *root = p._filterRoot.get();
        auto *pipeline = p._filterPSO.Get();
        const auto input = state.RawMipSRVs[base]->GPUHandle;
        const UINT samples = state.Source.PrefilterSamples;
        for (UINT mip = 0; mip < mips; ++mip)
        {
            const UINT size = p._resolution >> mip;
            const float roughness = float(mip) / (mips - 1);
            const auto output = p._filterUAVs[base + mip]->GPUHandle;
            work.Add(graph, "probe GGX prefilter mip " + std::to_string(mip),
                     {texture(state.Raw.get(), Mode::Read, read), texture(&p._filtered, Mode::Write, UnorderedAccess)},
                     [=](auto *cmd) {
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
        auto *source = p._filtered.GetResource()->D3DResource.Get();
        auto *destination = p._published.GetResource()->D3DResource.Get();
        const UINT publishedMips = p._publishedMips;
        publicationOperations.push_back(
            work.Add(graph, "probe publication copy",
                     {texture(&p._filtered, Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE),
                      texture(&p._published, Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST)},
                     [=](auto *cmd) {
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
        _publishing[_mode].push_back(probe);
        if (!p._settings.Scheduled)
            p._roundRobinProbe = (probe + 1) % UINT(p._probes.size());
    }
    for (UINT i = 0; i < MaxReflectionProbes; ++i)
    {
        GDX12ReflectionProbeData data;
        if (p._enabled && i < p._probes.size() && p._probes[i].Ready)
        {
            const auto &state = p._probes[i];
            const auto &source = state.Published;
            data.Position = source.Position;
            data.BoxMin = source.BoxMin;
            data.BoxMax = source.BoxMax;
            data.BlendDistance = state.Source.BlendDistance;
            data.Intensity = state.Source.Intensity;
            data.MaxMip = float(p._publishedMips - 1);
            data.MinMip = float(p._publishedMips - MipCount(source.Resolution));
        }
        constants.ReflectionProbesUpload->CopyData(i, data);
    }
    auto *source = constants.ReflectionProbesUpload->GetResource().D3DResource.Get();
    auto *target = constants.ReflectionProbes->GetResource().D3DResource.Get();
    work.Add(
        graph, "probe metadata upload (GPU ordered)",
        {{resource(&constants.ReflectionProbesUpload->GetResource()), Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ},
         {resource(&constants.ReflectionProbes->GetResource()), Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST}},
        [source, target](auto *cmd) {
            cmd->CopyBufferRegion(target, 0, source, 0, UINT64(MaxReflectionProbes) * sizeof(GDX12ReflectionProbeData));
        },
        publicationOperations);
    auto &evidence = _evidence[_mode].at(slot);
    evidence.Images.clear();
    evidence.Captured = p._statistics.CapturedFaces;
    evidence.Baked = p._statistics.BakedFaces;
    evidence.Published = p._statistics.PublishedProbes;
    const UINT64 metadataBytes = UINT64(MaxReflectionProbes) * sizeof(GDX12ReflectionProbeData);
    UINT64 bytes = (metadataBytes + 511) & ~UINT64(511);
    const auto shape = p._published.GetResource()->D3DResource->GetDesc();
    // Only logically published mip ranges are valid data. Pending faces,
    // unpublished probes and unused higher-resolution mips are not read back.
    for (UINT probe = 0; probe < p._probes.size(); ++probe)
        if (p._enabled && p._probes[probe].Ready)
        {
            const UINT minimum = p._publishedMips - MipCount(p._probes[probe].Published.Resolution);
            for (UINT face = 0; face < 6; ++face)
                for (UINT mip = minimum; mip < p._publishedMips; ++mip)
                {
                    Image image{};
                    image.Subresource = (probe * 6 + face) * p._publishedMips + mip;
                    UINT64 size = 0;
                    _resources.Device->GetDevice()->GetCopyableFootprints(
                        &shape, image.Subresource, 1, 0, &image.Footprint, &image.Rows, &image.RowBytes, &size);
                    image.Footprint.Offset = bytes;
                    bytes = (bytes + size + 511) & ~UINT64(511);
                    evidence.Images.push_back(image);
                }
        }
    if (evidence.Bytes != bytes)
    {
        evidence.Bytes = bytes;
        evidence.Buffer = _resources.Device->CreateResource(
            D3D12_HEAP_TYPE_READBACK, CD3DX12_RESOURCE_DESC::Buffer(bytes), D3D12_RESOURCE_STATE_COPY_DEST);
    }
    auto *metadata = constants.ReflectionProbes->GetResource().D3DResource.Get();
    auto *radiance = p._published.GetResource()->D3DResource.Get();
    auto *readback = evidence.Buffer.Get();
    const auto images = evidence.Images;
    std::vector<Access> accesses{
        {resource(&constants.ReflectionProbes->GetResource()), Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE},
        {NativeWork::ResourceFor(graph, bindings, readback, "probe readback"), Mode::Write,
         D3D12_RESOURCE_STATE_COPY_DEST}};
    if (!images.empty())
        accesses.push_back(texture(&p._published, Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE));
    work.Add(graph, "published probe metadata/mip readback", accesses, [=](auto *cmd) {
        cmd->CopyBufferRegion(readback, 0, metadata, 0, metadataBytes);
        for (const auto &image : images)
        {
            const CD3DX12_TEXTURE_COPY_LOCATION src(radiance, image.Subresource), dst(readback, image.Footprint);
            cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
    });
    p._planPrepared = false;
}
std::vector<uint8_t> SceneReflections::Readback(UINT slot, const std::string &stem) const
{
    const auto &evidence = _evidence[_mode].at(slot);
    const D3D12_RANGE read{0, SIZE_T(evidence.Bytes)}, noWrite{};
    void *mapped = nullptr;
    ThrowIfFailed(evidence.Buffer->Map(0, &read, &mapped));
    const auto *data = static_cast<const uint8_t *>(mapped);
    const UINT64 metadataBytes = UINT64(MaxReflectionProbes) * sizeof(GDX12ReflectionProbeData);
    std::vector<uint8_t> result(data, data + metadataBytes);
    for (const auto &image : evidence.Images)
        for (UINT y = 0; y < image.Rows; ++y)
        {
            const auto *row = data + image.Footprint.Offset + UINT64(y) * image.Footprint.Footprint.RowPitch;
            result.insert(result.end(), row, row + image.RowBytes);
        }
    evidence.Buffer->Unmap(0, &noWrite);
    if (!stem.empty())
    {
        std::ofstream out(stem + "-probes.bin", std::ios::binary);
        out.write(reinterpret_cast<const char *>(result.data()), result.size());
        std::ofstream meta(stem + "-probes.txt");
        meta << "captured=" << evidence.Captured << " baked=" << evidence.Baked << " published=" << evidence.Published
             << " image_subresources=" << evidence.Images.size() << " metadata_bytes=" << metadataBytes << "\n";
        for (const auto &image : evidence.Images)
            meta << "subresource=" << image.Subresource << " width=" << image.Footprint.Footprint.Width
                 << " height=" << image.Rows << " row_bytes=" << image.RowBytes << "\n";
    }
    return result;
}

} // namespace Meta

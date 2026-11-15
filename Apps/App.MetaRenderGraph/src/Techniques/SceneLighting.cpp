#include "SceneLighting.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include <FidelityFX/host/ffx_sssr.h>
#include <FidelityFX/host/backends/dx12/ffx_dx12.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
namespace Meta
{
namespace
{
FfxResource Import(GDX12Texture *texture, FfxResourceStates state = FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ)
{
    auto *resource = texture->GetResource()->D3DResource.Get();
    return ffxGetResourceDX12(resource, ffxGetResourceDescriptionDX12(resource), L"Meta SSSR", state);
}
void MatrixBytes(float *target, const Matrix &value)
{
    std::memcpy(target, &value, sizeof(value));
}
} // namespace
SceneLighting::SceneLighting(GDX12DeviceResources &r, RenderPipelineCommonData &c, bool opaqueOnly)
    : _resources(r), _common(c), _opaqueOnly(opaqueOnly), _frames(r.FrameConstants.size())
{
    if (opaqueOnly && r.FrameConstants.size() > FFX_MAX_QUEUED_FRAMES)
        throw std::logic_error("SSSR queued-frame contract exceeded");
    if (!opaqueOnly) _reflections = std::make_unique<SceneReflections>(r, c);
    if (opaqueOnly) _sunShadows = std::make_unique<SceneSunShadows>(r, c);
    if (opaqueOnly)
    {
        _localShadows = std::make_unique<SceneLocalShadows>(r, c);
        _transparency = std::make_unique<SceneTransparency>(r, c);
        GDX12RootSignatureDesc root;
        root.Constants = {9};
        root.NumSingleCBVSlots = 2;
        root.NumSingleSRVSlots = 1;
        root.NumSingleUAVSlots = 1;
        _gridRoot = std::make_unique<GDX12RootSignature>(r.Device, root);
        D3D12_FEATURE_DATA_D3D12_OPTIONS1 options{};
        const bool waves = SUCCEEDED(r.Device->GetDevice()->CheckFeatureSupport(
            D3D12_FEATURE_D3D12_OPTIONS1, &options, sizeof(options))) && options.WaveOps;
        const D3D_SHADER_MACRO defines[] = {{"LIGHT_GRID_WAVE_OPS", waves ? "1" : "0"}, {nullptr, nullptr}};
        _gridBuild = GDX12RenderHelpers::CreateComputePipeline(r.Device, _gridRoot.get(), SHADERS_FOLDER "LightGrid.hlsl", "BuildCS", defines);
    }
    _deferred._opaqueOnly = opaqueOnly;
    _deferred.SetFlag(RENDER_PASS_FLAG_USE_DOWNSCALED_RESOLUTION,true);
    _composition.SetFlag(RENDER_PASS_FLAG_USE_DOWNSCALED_RESOLUTION,true);
}
SceneLighting::~SceneLighting()
{
    for (auto &pass : _ssr)
        if (pass)
            pass->ClearDenendencies();
    if (_initialized)
    {
        _composition.ClearDenendencies();
        _deferred.ClearDenendencies();
    }
}
void SceneLighting::Reset()
{
    _pendingReset = true;
}
void SceneLighting::Freeze(SceneGeometry &geometry, UINT slot)
{
    if (_pendingReset)
    {
        if (_reflections) _reflections->Reset();
        _forceReset = {true, true};
        _pendingReset = false;
    }
    if (_reflections) _reflections->Freeze();
    _ssrSettings = GetSSRSettings();
    _gridSettings = GetLightGridSettings();
    _slot = slot;
    auto &scene = geometry._slots.at(slot);
    auto &frame = _frames.at(slot);
    frame.Width = scene.DepthView->GetWidth();
    frame.Height = scene.DepthView->GetHeight();
    auto *normals = scene.EarlyAO ? scene.PrepassNormals.get() : scene.Color[2].get();
    if (!frame.AO)
        frame.AO = std::make_unique<SceneAO>(_resources, _common, scene.DepthView.get(), normals);
    frame.AO->Freeze(scene.DepthView.get(), normals, _aoTemporalIndex);
    if (!_opaqueOnly)
    {
        // The existing CPU cache builder supplies static/dynamic probe decisions.
        // Its old GPU upload/Execute paths are not called by this executor.
        _resources.PrepareLocalShadowData();
    }
    auto &constants = *_resources.FrameConstants.at(slot);
    const auto grass = geometry.GrassPrimitives();
    if (_localShadows)
    {
        _localShadows->Freeze(slot, grass);
    }
    if (_sunShadows) _sunShadows->Freeze(slot, _localShadows->CastersRebuilt(),
        _localShadows->ChangedCasterBounds(), grass);
    constants.ShadowCB->CopyData(0, _resources.ShadowSettings);
    if (!constants.LocalLightCache->GetElementCount())
    {
        constants.LocalLightCache->Resize(1);
        constants.LocalLightCache->CopyData(0, GDX12LocalLightConstants{});
    }
    if (_localShadows) _localShadows->Complete(slot);
    auto& lightSource = _localShadows ? _localShadows->LightSource(slot) : *constants.LocalLightCache;
    const UINT count = lightSource.GetElementCount();
    if (!constants.ShadowLights)
    {
        constants.ShadowLights = std::make_unique<GDX12UploadBuffer<GDX12LocalLightConstants>>(
            _resources.Device, count, EBufferType::Default, false);
        constants.ShadowLights->CreateSRV(_resources.SRV_UAV_Heap.get(),
                                          _resources.SRV_UAV_Heap->GetAvailableIndex(105000));
    }
    else
        constants.ShadowLights->Resize(count);
    if (_opaqueOnly)
    {
        auto& source = lightSource;
        frame.UploadLights = frame.LightRevision != source.GetRevision() ||
            frame.LightSource.Get() != source.GetResource().D3DResource.Get();
        if (frame.UploadLights)
        {
            frame.LightRevision = source.GetRevision();
            frame.LightSource = source.GetResource().D3DResource;
        }
    }
}

void SceneLighting::AppendGpuLightGrid(Graph& graph, std::vector<ID3D12Resource*>& bindings, const SceneGeometry& geometry)
{
    const auto& scene = geometry._slots.at(_slot);
    const auto& frame = _frames.at(_slot);
    auto& grid = _deferred._grids.at(_slot);
    const auto layout = BuildLightGridConstants(_common.CameraProjection, _common.CameraView,
        _common.ActiveCameraNearPlane, _common.ActiveCameraFarPlane, frame.Width, frame.Height,
        _resources.LocalLights.size(), _gridSettings);
    const UINT words = UINT((std::max)(size_t(1), size_t(layout.Size.x) * layout.Size.y * layout.Size.z * layout.Size.w));
    grid.Masks->Resize(words);
    grid.Constants->CopyData(0, layout);
    const auto resource = [&](GDX12Resource& value) {
        return NativeWork::ResourceFor(graph, bindings, value.D3DResource.Get(), "GPU light grid resource");
    };
    const Id masks = resource(grid.Masks->GetResource());
    const Id constants = resource(grid.Constants->GetResource());
    auto& lights = *_resources.FrameConstants.at(_slot)->ShadowLights;
    const Id sources = resource(lights.GetResource());
    const UINT groups = layout.Size.w ? ((layout.Size.x * layout.Size.y + 63) / 64) * layout.Size.z * layout.Size.w : 1;
    std::array<UINT, 9> parameters{frame.Width, frame.Height, UINT(_resources.LocalLights.size()), groups};
    const float projection[] = {_common.CameraProjection._11, _common.CameraProjection._22,
        _common.CameraProjection._31, _common.CameraProjection._32, _common.ActiveCameraFarPlane};
    std::memcpy(parameters.data() + 4, projection, sizeof(projection));
    auto* root = _gridRoot.get();
    auto* heap = _resources.SRV_UAV_Heap->GetHeap().Get();
    auto* build = _gridBuild.Get();
    const auto camera = scene.Camera;
    const auto gridCB = grid.Constants->GetElementAddress(0);
    const auto srv = lights.GetSRV()->GPUHandle;
    const auto uav = grid.Masks->GetUAV()->GPUHandle;
    _work[_mode].Add(graph, "GPU light grid masks",
        {{masks, Mode::Write, UnorderedAccess}, {sources, Mode::Read, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE},
         {scene.FrameIds[1], Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ},
         {constants, Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ}}, [=](auto* cmd) {
        cmd->SetDescriptorHeaps(1, &heap);
        cmd->SetComputeRootSignature(root->GetRootSignature().Get());
        cmd->SetPipelineState(build);
        cmd->SetComputeRoot32BitConstants(0, UINT(parameters.size()), parameters.data(), 0);
        cmd->SetComputeRootConstantBufferView(root->GetBRootParamIndex(1), camera);
        cmd->SetComputeRootConstantBufferView(root->GetBRootParamIndex(2), gridCB);
        cmd->SetComputeRootDescriptorTable(root->GetTRootParamIndex(0), srv);
        cmd->SetComputeRootDescriptorTable(root->GetURootParamIndex(0), uav);
        cmd->Dispatch((std::min)(groups, 65535u), (groups + 65534) / 65535, 1);
    });
}
void SceneLighting::AppendAO(Graph &graph, std::vector<ID3D12Resource *> &bindings)
{
    _frames.at(_slot).AO->Append(graph, bindings);
}
bool SceneLighting::Record(Id id, ID3D12GraphicsCommandList *commands) const
{
    return _work[_mode].Record(id, commands) || (_sunShadows && _sunShadows->Record(_slot, id, commands)) ||
           (_localShadows && _localShadows->Record(_slot, id, commands)) ||
           (_transparency && _transparency->Record(id, commands)) ||
           (_reflections && _reflections->Record(id, commands)) ||
           (_frames.at(_slot).AO && _frames.at(_slot).AO->Record(id, commands));
}
void SceneLighting::Submitted(bool serial, uint64_t fence)
{
    if (_localShadows) _localShadows->Submitted(_slot);
    if (_reflections) _reflections->Submitted(serial, fence);
}
void SceneLighting::Append(Graph &graph, std::vector<ID3D12Resource *> &bindings, SceneGeometry &geometry,
                           SceneEnvironment &environment, GDX12Texture *target, bool serial)
{
    _mode = serial ? 0 : 1;
    auto &work = _work[_mode];
    work.Commands.clear();
    auto &scene = geometry._slots.at(_slot);
    auto &frame = _frames.at(_slot);
    auto &constants = *_resources.FrameConstants[_slot];
    if (_opaqueOnly) _transparency->PrepareWater(graph, bindings, geometry, _slot);
    const auto resource = [&](GDX12Resource *value) {
        return NativeWork::ResourceFor(graph, bindings, value->D3DResource.Get(), "lighting/SSR resource");
    };
    const auto texture = [&](GDX12Texture *value, Mode mode, State state) {
        return Access{resource(value->GetResource()), mode, state};
    };
    const State read = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    auto *heap = _resources.SRV_UAV_Heap->GetHeap().Get();
    const auto appendGrass = [&](const GeometryTarget& view, Graph& g, std::vector<ID3D12Resource*>& resources) {
        geometry.AppendGrassView(g, resources, _slot, view);
    };
    if (_sunShadows) _sunShadows->Append(graph, bindings, _slot,
        {scene.Buffers[0].Srv.GPUHandle, scene.Buffers[1].Srv.GPUHandle,
         scene.Buffers[2].Srv.GPUHandle, scene.Buffers[3].Srv.GPUHandle}, scene.Draws, scene.Geometry, appendGrass);
    auto* lights = constants.ShadowLights.get();
    if (!_opaqueOnly || frame.UploadLights)
    {
        auto& source = _localShadows ? _localShadows->LightSource(_slot) : *constants.LocalLightCache;
        auto *lightSource = source.GetResource().D3DResource.Get();
        auto *lightTarget = constants.ShadowLights->GetResource().D3DResource.Get();
        const UINT64 lightBytes = UINT64(source.GetElementCount()) * sizeof(GDX12LocalLightConstants);
        work.Add(graph, "local-light snapshot upload",
                 {{resource(&source.GetResource()), Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ},
                  {resource(&constants.ShadowLights->GetResource()), Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST}},
                 [=](auto *cmd) { cmd->CopyBufferRegion(lightTarget, 0, lightSource, 0, lightBytes); });
    }
    if (_localShadows) _localShadows->Append(graph, bindings, _slot,
        {scene.Buffers[0].Srv.GPUHandle, scene.Buffers[1].Srv.GPUHandle,
         scene.Buffers[2].Srv.GPUHandle, scene.Buffers[3].Srv.GPUHandle}, scene.Geometry, *constants.ShadowLights, appendGrass);
    if (!_opaqueOnly)
    {
        geometry._shadows->AppendLighting(graph, bindings, _slot);
        _reflections->Append(graph, bindings, geometry, environment, _slot, serial);
    }
    if (!scene.EarlyAO)
        AppendAO(graph, bindings);
    _deferred.Setup(&_resources, &_common);
    _deferred.SetInputs({scene.Color[0].get(), scene.Color[2].get(), scene.Color[3].get(), scene.Color[4].get(),
                         scene.DepthView.get(), _sunShadows ? _sunShadows->Output(_slot) : geometry._shadows->LightingSun(_slot), environment.Output(0),
                         environment.Output(1), environment.Output(2), environment.Output(3),
                         _localShadows ? _localShadows->Output(_slot) : geometry._shadows->LightingLocal(_slot),
                         frame.AO->Output()});
    if (!_initialized)
    {
        _deferred.Initialize();
        if (_opaqueOnly) for (auto& grid : _deferred._grids)
            grid.Masks->CreateUAV(_resources.SRV_UAV_Heap.get(),
                _resources.SRV_UAV_Heap->GetAvailableIndex(105000));
        if (!_opaqueOnly)
        {
            GDX12RenderHelpers::InitializeRenderTarget(_neutralAccum, _resources, frame.Width, frame.Height,
                                                       DXGI_FORMAT_R16G16B16A16_FLOAT, {0, 0, 0, 0});
            GDX12RenderHelpers::InitializeRenderTarget(_neutralReveal, _resources, frame.Width, frame.Height,
                                                       DXGI_FORMAT_R16_FLOAT, {1, 1, 1, 1});
        }
    }
    else if (_deferred._color->GetWidth() != frame.Width || _deferred._color->GetHeight() != frame.Height)
    {
        // Shared size-dependent HDR/SDK resources require an owner drain; this
        // is a concrete resize section, never a recording-worker wait.
        _resources.Device->GetCommandQueue()->Flush();
        _deferred.Resize();
        if (!_opaqueOnly)
        {
            _neutralAccum.Resize(frame.Width, frame.Height);
            _neutralReveal.Resize(frame.Width, frame.Height);
            _composition.Resize();
        }
        _forceReset = {true, true};
    }
    auto &grid = _deferred._grids.at(_slot);
    auto* masks = grid.Masks.get();
    if (_opaqueOnly) AppendGpuLightGrid(graph, bindings, geometry);
    else
    {
        _deferred._gridSystem.Build(_deferred._grid, _common.CameraProjection, _common.CameraView,
            _common.ActiveCameraNearPlane, _common.ActiveCameraFarPlane, frame.Width, frame.Height,
            _resources.LocalLights, _common.Jobs, _gridSettings);
        const UINT words = UINT(_deferred._grid.Masks.size());
        grid.Upload->Resize(words);
        grid.Masks->Resize(words);
        if (words)
            grid.Upload->CopyData(0, _deferred._grid.Masks.data(), words);
        grid.Constants->CopyData(0, _deferred._grid.Constants);
        const UINT64 gridBytes = UINT64(words) * sizeof(UINT);
        auto *gridSource = grid.Upload->GetResource().D3DResource.Get();
        auto *gridTarget = grid.Masks->GetResource().D3DResource.Get();
        work.Add(graph, "clustered-light mask upload",
                 {{resource(&grid.Upload->GetResource()), Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ},
                  {resource(&grid.Masks->GetResource()), Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST}},
                 [=](auto *cmd) {
                     if (gridBytes)
                         cmd->CopyBufferRegion(gridTarget, 0, gridSource, 0, gridBytes);
                 });
    }
    _deferred._prepared = true;
    // Placed RT/UAV initialization has its own declared state and packet.
    auto *hdr = _deferred._color->GetResource()->D3DResource.Get();
    work.Add(graph, "initialize deferred HDR target",
             {texture(_deferred._color.get(), Mode::Write, D3D12_RESOURCE_STATE_RENDER_TARGET)},
             [hdr](auto *cmd) { cmd->DiscardResource(hdr, nullptr); });
    auto lighting = environment.Reads(graph, bindings);
    for (UINT i = 0; i < _deferred._inputs.size(); ++i)
        lighting.push_back(texture(_deferred._inputs[i]->GetTexture(), Mode::Read, read));
    if (_reflections)
    {
        lighting.push_back(texture(_reflections->Published(serial), Mode::Read, read));
        lighting.push_back({resource(&constants.ReflectionProbes->GetResource()), Mode::Read, read});
    }
    lighting.push_back({resource(&lights->GetResource()), Mode::Read, read});
    lighting.push_back({resource(&masks->GetResource()), Mode::Read, read});
    lighting.push_back({resource(&grid.Constants->GetResource()), Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ});
    lighting.push_back({resource(&constants.ShadowCB->GetResource()), Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ});
    lighting.push_back({resource(&constants.SkyCB->GetResource()), Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ});
    lighting.push_back({scene.FrameIds[0], Mode::Read, _opaqueOnly ? State(D3D12_RESOURCE_STATE_GENERIC_READ) : State(D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER)});
    lighting.push_back({scene.FrameIds[1], Mode::Read, _opaqueOnly ? State(D3D12_RESOURCE_STATE_GENERIC_READ) : State(D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER)});
    lighting.push_back(texture(_deferred._color.get(), Mode::Write, UnorderedAccess));
    // Environment outputs can occur in both the explicit pass inputs and LUT
    // reads; normalize identical physical reads, rejecting state disagreements.
    const auto unique = [](std::vector<Access> accesses) {
        std::vector<Access> result;
        for (auto a : accesses)
        {
            auto found = std::find_if(result.begin(), result.end(), [&](auto b) { return a.Resource == b.Resource; });
            if (found == result.end())
                result.push_back(a);
            else if (a.Usage != found->Usage || a.Required != found->Required)
                throw std::logic_error("Conflicting lighting access");
        }
        return result;
    };
    auto *root = _deferred._root.get();
    auto *pipeline = _deferred._pipeline.Get();
    std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 18> inputs{};
    for (UINT i = 0; i < 12; ++i)
        inputs[i < 11 ? i : 14] = _deferred._inputs[i]->GetTexture()->GetSRV()->GPUHandle;
    inputs[11] = lights->GetSRV()->GPUHandle;
    if (_reflections) inputs[12] = _reflections->Radiance(serial);
    inputs[13] = constants.ReflectionProbes->GetSRV()->GPUHandle;
    inputs[15] = _resources.SkyTransmittanceSRV.GPUHandle;
    inputs[16] = _resources.SkyViewSRV.GPUHandle;
    inputs[17] = masks->GetSRV()->GPUHandle;
    const std::array<D3D12_GPU_VIRTUAL_ADDRESS, 5> cbvs{
        scene.Main, scene.Camera, constants.ShadowCB->GetElementAddress(0), constants.SkyCB->GetElementAddress(0),
        grid.Constants->GetElementAddress(0)};
    const auto output = _deferred._color->GetUAV()->GPUHandle;
    const UINT width = frame.Width, height = frame.Height;
    const bool opaqueOnly = _opaqueOnly;
    work.Add(graph, "clustered deferred lighting", unique(lighting), [=](auto *cmd) {
        cmd->SetDescriptorHeaps(1, &heap);
        cmd->SetComputeRootSignature(root->GetRootSignature().Get());
        cmd->SetPipelineState(pipeline);
        for (UINT i = 0; i < 5; ++i)
            cmd->SetComputeRootConstantBufferView(root->GetBRootParamIndex(i), cbvs[i]);
        for (UINT i = 0; i < 18; ++i)
            if (!opaqueOnly || (i != 12 && i != 13))
                cmd->SetComputeRootDescriptorTable(root->GetTRootParamIndex(i), inputs[i]);
        cmd->SetComputeRootDescriptorTable(root->GetURootParamIndex(0), output);
        cmd->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    });
    const auto appendOrdinary = [&](GDX12Texture* opaqueHDR) {
        _initialized = true;
        if (!_hdrContinuation) throw std::logic_error("Opaque lighting requires a display continuation");
        auto* hdr = _transparency->Append(graph, bindings, geometry, _slot, opaqueHDR,
            _sunShadows->Output(_slot), _localShadows->Output(_slot), environment);
        _hdrContinuation(graph, bindings, hdr, nullptr);
    };
    if (_opaqueOnly && !_ssrSettings.Enabled)
    {
        _forceReset[_mode] = true;
        appendOrdinary(_deferred._color.get());
        return;
    }
    auto &ssr = _ssr[_mode];
    const std::vector<IRenderPassLink *> ssrInputs{_deferred._color.get(), scene.DepthView.get(), scene.Color[2].get(),
                                                   scene.Color[3].get(),   scene.Color[1].get(),  environment.Output(1),
                                                   environment.Output(2)};
    if (!ssr)
    {
        ssr = std::make_unique<GDX12SSSRPass>();
        ssr->SetStaticIBLOnly(_opaqueOnly);
        ssr->Setup(&_resources, &_common);
        ssr->SetInputs(ssrInputs);
        ssr->Initialize();
    }
    else
    {
        ssr->SetInputs(ssrInputs);
        if (ssr->_color.GetWidth() != width || ssr->_color.GetHeight() != height)
        {
            ssr->Resize();
            _forceReset[_mode] = true;
        }
    }
    auto &p = *ssr;
    const auto settings = _ssrSettings;
    const bool settingsChanged = settings != p._previousSettings;
    if (settingsChanged && (settings.Enabled || p._previousSettings.Enabled))
        _common.ResetUpscalerHistory = true;
    if (!settings.Enabled)
    {
        auto *source = _deferred._color->GetResource()->D3DResource.Get();
        auto *destination = p._color.GetResource()->D3DResource.Get();
        work.Add(graph, "SSR disabled copy-through",
                 {texture(_deferred._color.get(), Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE),
                  texture(&p._color, Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST)},
                 [=](auto *cmd) { cmd->CopyResource(destination, source); });
        p._previousSettings = settings;
        _forceReset[_mode] = true;
    }
    else
    {
        const Matrix view = _common.CameraView, projection = _common.CameraProjection,
                     viewProjection = view * projection;
        const XMFLOAT2 jitter{_common.ActiveCameraJitterOffsetX, _common.ActiveCameraJitterOffsetY};
        const auto history = _opaqueOnly ? _common.HistoryRevision : _reflections->History(serial);
        // Frame-local GBuffer addresses rotate with frame slots. They are not
        // history discontinuities. The SDK's private denoiser history persists
        // across those imports and is retired/recreated only on real resets.
        bool contextCreated = false;
        if (!p._context || _forceReset[_mode] || settingsChanged || p._historyRevision != history ||
            p._environmentRevision != _resources.ReflectionEnvironmentRevision)
        {
            p.CreateContext();
            contextCreated = true;
            p._previousViewProjection = viewProjection;
            p._previousJitter = jitter;
        }
        if (!_opaqueOnly && _resources.FrameConstants.size() > FFX_MAX_QUEUED_FRAMES)
            _resources.Device->GetCommandQueue()->Flush();
        GDX12SSSRPass::Constants parameters;
        parameters.JitterDeltaUV = {(p._previousJitter.x - jitter.x) / width,
                                    (p._previousJitter.y - jitter.y) / height};
        parameters.RoughnessCutoff = settings.RoughnessCutoff;
        p._constants->CopyData(_slot, parameters);
        auto *ssrRoot = p._root.get();
        auto *prepare = p._prepare.Get();
        auto *resolve = p._resolve.Get();
        const auto camera = scene.Camera, ssrCB = p._constants->GetElementAddress(_slot);
        std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 11> srvs{};
        for (UINT i = 0; i < 7; ++i)
            srvs[i] = p._inputs[i]->GetTexture()->GetSRV()->GPUHandle;
        srvs[7] = p._radiance.GetSRV()->GPUHandle;
        srvs[8] = p._fallback.GetSRV()->GPUHandle;
        if (_reflections)
        {
            srvs[9] = _reflections->Radiance(serial);
            srvs[10] = constants.ReflectionProbes->GetSRV()->GPUHandle;
        }
        const UINT srvCount = _opaqueOnly ? 9 : 11;
        const auto bind = [=](auto *cmd, bool resolvedInputs) {
            cmd->SetDescriptorHeaps(1, &heap);
            cmd->SetComputeRootSignature(ssrRoot->GetRootSignature().Get());
            cmd->SetComputeRootConstantBufferView(ssrRoot->GetBRootParamIndex(0), camera);
            cmd->SetComputeRootConstantBufferView(ssrRoot->GetBRootParamIndex(1), ssrCB);
            for (UINT i = 0; i < srvCount; ++i)
                if(resolvedInputs || (i!=7 && i!=8))
                    cmd->SetComputeRootDescriptorTable(ssrRoot->GetTRootParamIndex(i), srvs[i]);
        };
        std::vector<Access> ssrReads;
        for (auto *input : p._inputs)
            ssrReads.push_back(texture(input->GetTexture(), Mode::Read, read));
        if (_reflections)
        {
            ssrReads.push_back(texture(_reflections->Published(serial), Mode::Read, read));
            ssrReads.push_back({resource(&constants.ReflectionProbes->GetResource()), Mode::Read, read});
        }
        ssrReads.push_back({scene.FrameIds[1], Mode::Read, _opaqueOnly ? State(D3D12_RESOURCE_STATE_GENERIC_READ) : State(D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER)});
        ssrReads.push_back({resource(&p._constants->GetResource()), Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ});
        auto prepareAccess = ssrReads;
        prepareAccess.push_back(texture(&p._roughness, Mode::Write, UnorderedAccess));
        prepareAccess.push_back(texture(&p._motion, Mode::Write, UnorderedAccess));
        prepareAccess.push_back(texture(&p._fallback, Mode::Write, UnorderedAccess));
        const auto roughness = p._roughness.GetUAV()->GPUHandle, motion = p._motion.GetUAV()->GPUHandle,
                   fallback = p._fallback.GetUAV()->GPUHandle;
        work.Add(graph, "SSSR prepare roughness/motion/environment fallback", unique(prepareAccess), [=](auto *cmd) {
            bind(cmd,false);
            cmd->SetPipelineState(prepare);
            cmd->SetComputeRootDescriptorTable(ssrRoot->GetURootParamIndex(0), roughness);
            cmd->SetComputeRootDescriptorTable(ssrRoot->GetURootParamIndex(1), motion);
            cmd->SetComputeRootDescriptorTable(ssrRoot->GetURootParamIndex(3), fallback);
            cmd->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
        });
        FfxSssrDispatchDescription desc{};
        desc.color = Import(_deferred._color.get());
        desc.depth = Import(scene.DepthView.get());
        desc.normal = Import(scene.Color[2].get());
        desc.materialParameters = Import(&p._roughness, FFX_RESOURCE_STATE_COMPUTE_READ);
        desc.motionVectors = Import(&p._motion, FFX_RESOURCE_STATE_COMPUTE_READ);
        desc.environmentMap = Import(&p._fallback, FFX_RESOURCE_STATE_COMPUTE_READ);
        desc.brdfTexture = Import(environment.Output(2)->GetTexture());
        desc.output = Import(&p._radiance, FFX_RESOURCE_STATE_COMPUTE_READ);
        MatrixBytes(desc.invViewProjection, viewProjection.Invert());
        MatrixBytes(desc.projection, projection);
        MatrixBytes(desc.invProjection, projection.Invert());
        MatrixBytes(desc.view, view);
        MatrixBytes(desc.invView, view.Invert());
        MatrixBytes(desc.prevViewProjection, p._previousViewProjection);
        desc.renderSize = {width, height};
        desc.motionVectorScale = {1, 1};
        desc.iblFactor = 1;
        desc.normalUnPackMul = 1;
        desc.isRoughnessPerceptual = false;
        desc.temporalStabilityFactor = settings.TemporalStability;
        desc.depthBufferThickness = settings.Thickness;
        desc.roughnessThreshold = settings.RoughnessCutoff * settings.RoughnessCutoff;
        desc.varianceThreshold = settings.VarianceThreshold;
        desc.maxTraversalIntersections = settings.MaxIntersections;
        desc.minTraversalOccupancy = settings.MinOccupancy;
        desc.mostDetailedMip = (std::min)(UINT(settings.MostDetailedMip), UINT(std::log2((std::max)(width, height))));
        desc.samplesPerQuad = settings.SamplesPerQuad;
        desc.temporalVarianceGuidedTracingEnabled = settings.VarianceGuided;
        auto sdk = ssrReads;
        sdk.push_back(texture(&p._roughness, Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
        sdk.push_back(texture(&p._motion, Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
        sdk.push_back(texture(&p._fallback, Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
        sdk.push_back(texture(&p._radiance, Mode::ReadWrite, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
        auto *context = p._context.get();
        const auto sdkOperation = work.Add(graph, "SSSR + denoiser (exclusive persistent SDK history)", unique(sdk),
                                           [context, desc](auto *cmd) mutable {
                                               desc.commandList = ffxGetCommandListDX12(cmd);
                                               const auto result = ffxSssrContextDispatch(context, &desc);
                                               if (result != FFX_OK)
                                                   throw std::runtime_error("Native SSSR SDK dispatch failed");
                                           });
        // The bundled DX12 backend allocates/maps its fallback constant ring
        // on first ExecuteGpuJobs. Record that first real dispatch on the owner.
        // Subsequent dispatches reuse its private, queued-frame-safe ring.
        graph.Operations.at(sdkOperation).OwnerRecording = contextCreated;
        auto resolveAccess = ssrReads;
        resolveAccess.push_back(texture(&p._radiance, Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
        resolveAccess.push_back(texture(&p._fallback, Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
        resolveAccess.push_back(texture(&p._color, Mode::Write, UnorderedAccess));
        const auto color = p._color.GetUAV()->GPUHandle;
        work.Add(graph, "SSR specular composition", unique(resolveAccess), [=](auto *cmd) {
            bind(cmd,true);
            cmd->SetPipelineState(resolve);
            cmd->SetComputeRootDescriptorTable(ssrRoot->GetURootParamIndex(2), color);
            cmd->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
        });
        p._previousViewProjection = viewProjection;
        p._previousJitter = jitter;
        p._historyRevision = history;
        p._environmentRevision = _resources.ReflectionEnvironmentRevision;
        p._previousSettings = settings;
        _forceReset[_mode] = false;
    }
    if (_opaqueOnly)
    {
        appendOrdinary(&p._color);
        return;
    }
    _composition.Setup(&_resources, &_common);
    _composition.SetInputs({&p._color, &_neutralAccum, &_neutralReveal, scene.DepthView.get()});
    if (!_initialized)
    {
        _composition.Initialize();
        _initialized = true;
    }
    const auto neutralAccum = _neutralAccum.GetRTV()->CPUHandle, neutralReveal = _neutralReveal.GetRTV()->CPUHandle;
    work.Add(graph, "opaque-only composition neutral transparency",
             {texture(&_neutralAccum, Mode::Write, D3D12_RESOURCE_STATE_RENDER_TARGET),
              texture(&_neutralReveal, Mode::Write, D3D12_RESOURCE_STATE_RENDER_TARGET)},
             [=](auto *cmd) {
                 const float black[4] = {}, white[4] = {1, 1, 1, 1};
                 cmd->ClearRenderTargetView(neutralAccum, black, 0, nullptr);
                 cmd->ClearRenderTargetView(neutralReveal, white, 0, nullptr);
             });
    auto composition = environment.Reads(graph, bindings);
    composition.push_back(texture(&p._color, Mode::Read, read));
    composition.push_back(texture(&_neutralAccum, Mode::Read, read));
    composition.push_back(texture(&_neutralReveal, Mode::Read, read));
    composition.push_back(texture(scene.DepthView.get(), Mode::Read, read));
    composition.push_back({scene.FrameIds[1], Mode::Read, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER});
    composition.push_back({resource(&constants.SkyCB->GetResource()), Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ});
    composition.push_back(texture(_composition.OUT_Result.get(), Mode::Write, D3D12_RESOURCE_STATE_RENDER_TARGET));
    auto *compositionRoot = _composition._compositionRS.get();
    auto *compositionPSO = _composition._compositionPSO.Get();
    const auto composed = _composition.OUT_Result->GetRTV()->CPUHandle;
    const auto camera = scene.Camera, sky = constants.SkyCB->GetElementAddress(0);
    const std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 7> compositionInputs{p._color.GetSRV()->GPUHandle,
                                                                       _neutralAccum.GetSRV()->GPUHandle,
                                                                       _neutralReveal.GetSRV()->GPUHandle,
                                                                       scene.DepthView->GetSRV()->GPUHandle,
                                                                       _resources.SkyTransmittanceSRV.GPUHandle,
                                                                       _resources.SkyAerialRadianceSRV.GPUHandle,
                                                                       _resources.SkyAerialTransmittanceSRV.GPUHandle};
    work.Add(graph, "opaque aerial/HDR composition", unique(composition), [=](auto *cmd) {
        cmd->SetDescriptorHeaps(1, &heap);
        cmd->SetGraphicsRootSignature(compositionRoot->GetRootSignature().Get());
        cmd->SetPipelineState(compositionPSO);
        cmd->SetGraphicsRootConstantBufferView(compositionRoot->GetBRootParamIndex(0), camera);
        cmd->SetGraphicsRootConstantBufferView(compositionRoot->GetBRootParamIndex(1), sky);
        for (UINT i = 0; i < 7; ++i)
            cmd->SetGraphicsRootDescriptorTable(compositionRoot->GetTRootParamIndex(i), compositionInputs[i]);
        const D3D12_VIEWPORT vp{0, 0, float(width), float(height), 0, 1};
        const D3D12_RECT rect{0, 0, LONG(width), LONG(height)};
        cmd->RSSetViewports(1, &vp);
        cmd->RSSetScissorRects(1, &rect);
        cmd->OMSetRenderTargets(1, &composed, FALSE, nullptr);
        const float zero[4] = {};
        cmd->ClearRenderTargetView(composed, zero, 0, nullptr);
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmd->DrawInstanced(3, 1, 0, 0);
    });
    auto* displayHDR = _composition.OUT_Result.get();
    if (_hdrContinuation)
        displayHDR = _hdrContinuation(graph, bindings, &p._color, _reflections->Published(serial));
    if (!_hdrContinuation)
    {
    // A diagnostic display of intermediate HDR only; U7 reconstruction, output
    // providers, frame generation and the full post chain remain untouched.
    if (!_displayRoot)
    {
        GDX12RootSignatureDesc desc;
        desc.NumSingleSRVSlots = 1;
        desc.StaticSamplers = GetStaticSamplers();
        _displayRoot = std::make_unique<GDX12RootSignature>(_resources.Device, desc);
        auto &compiler = GDX12ShaderCompiler::GetInstance();
        const auto vs = compiler.CompileShader(_resources.Device, SHADERS_FOLDER "FullScreenVS.hlsl", nullptr, "VS",
                                               "vs"),
                   ps = compiler.CompileShader(_resources.Device, SHADERS_FOLDER "ToneMap.hlsl", nullptr, "PS", "ps");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC state{};
        state.pRootSignature = _displayRoot->GetRootSignature().Get();
        state.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        state.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        state.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        state.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        state.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        state.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        state.DepthStencilState.DepthEnable = false;
        state.DepthStencilState.StencilEnable = false;
        state.SampleMask = UINT_MAX;
        state.SampleDesc.Count = 1;
        state.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        state.NumRenderTargets = 1;
        state.RTVFormats[0] = target->GetFormat();
        ThrowIfFailed(
            _resources.Device->GetDevice()->CreateGraphicsPipelineState(&state, IID_PPV_ARGS(&_displayPipeline)));
    }
    auto *displayRoot = _displayRoot.get();
    auto *displayPSO = _displayPipeline.Get();
    const auto displayInput = displayHDR->GetSRV()->GPUHandle;
    const auto displayTarget = target->GetRTV()->CPUHandle;
    const auto viewport = target->GetViewport();
    const auto scissor = target->GetScissorRect();
    auto *nativeTarget = target->GetResource()->D3DResource.Get();
    work.Add(graph, "diagnostic intermediate HDR display",
             {texture(displayHDR, Mode::Read, read),
              texture(target, Mode::Write, D3D12_RESOURCE_STATE_RENDER_TARGET)},
             [=](auto *cmd) {
                 cmd->SetDescriptorHeaps(1, &heap);
                 cmd->SetGraphicsRootSignature(displayRoot->GetRootSignature().Get());
                 cmd->SetPipelineState(displayPSO);
                 cmd->SetGraphicsRootDescriptorTable(displayRoot->GetTRootParamIndex(0), displayInput);
                 cmd->DiscardResource(nativeTarget, nullptr);
                 cmd->RSSetViewports(1, &viewport);
                 cmd->RSSetScissorRects(1, &scissor);
                 cmd->OMSetRenderTargets(1, &displayTarget, FALSE, nullptr);
                 cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                 cmd->DrawInstanced(3, 1, 0, 0);
             });
    }
    const std::array<GDX12Texture *, 4> images{frame.AO->Output(), _deferred._color.get(), &p._color,
                                               _composition.OUT_Result.get()};
    UINT64 bytes = 0;
    std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, 4> footprints{};
    for (UINT i = 0; i < 4; ++i)
    {
        const auto shape = images[i]->GetResource()->D3DResource->GetDesc();
        UINT64 size = 0;
        _resources.Device->GetDevice()->GetCopyableFootprints(&shape, 0, 1, 0, &footprints[i], nullptr, nullptr, &size);
        footprints[i].Offset = bytes;
        bytes = (bytes + size + 511) & ~UINT64(511);
    }
    if (frame.Bytes != bytes)
    {
        frame.Bytes = bytes;
        frame.Readback = _resources.Device->CreateResource(
            D3D12_HEAP_TYPE_READBACK, CD3DX12_RESOURCE_DESC::Buffer(bytes), D3D12_RESOURCE_STATE_COPY_DEST);
    }
    frame.Footprints = footprints;
    std::vector<Access> readback{{NativeWork::ResourceFor(graph, bindings, frame.Readback.Get(), "lighting readback"),
                                  Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST}};
    std::array<ID3D12Resource *, 4> sources{};
    for (UINT i = 0; i < 4; ++i)
    {
        readback.push_back(texture(images[i], Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE));
        sources[i] = images[i]->GetResource()->D3DResource.Get();
    }
    auto *destination = frame.Readback.Get();
    work.Add(graph, "AO/HDR/SSR/composition readback", readback, [=](auto *cmd) {
        for (UINT i = 0; i < 4; ++i)
        {
            const CD3DX12_TEXTURE_COPY_LOCATION src(sources[i], 0), dst(destination, footprints[i]);
            cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
    });
}
std::vector<uint8_t> SceneLighting::Readback(UINT slot, const std::string &stem) const
{
    const auto &frame = _frames.at(slot);
    const D3D12_RANGE read{0, SIZE_T(frame.Bytes)}, noWrite{};
    void *mapped = nullptr;
    ThrowIfFailed(frame.Readback->Map(0, &read, &mapped));
    const auto *data = static_cast<const uint8_t *>(mapped);
    std::vector<uint8_t> result;
    for (UINT i = 0; i < 4; ++i)
    {
        const UINT pixelBytes = i == 0 ? 4 : 8;
        const auto &fp = frame.Footprints[i];
        for (UINT y = 0; y < frame.Height; ++y)
        {
            const auto *row = data + fp.Offset + UINT64(y) * fp.Footprint.RowPitch;
            result.insert(result.end(), row, row + UINT64(frame.Width) * pixelBytes);
        }
    }
    frame.Readback->Unmap(0, &noWrite);
    const auto probes = _reflections->Readback(slot, stem);
    result.insert(result.end(), probes.begin(), probes.end());
    if (!stem.empty())
    {
        std::ofstream out(stem + "-lighting.bin", std::ios::binary);
        out.write(reinterpret_cast<const char *>(result.data()), result.size());
        std::ofstream meta(stem + "-lighting.txt");
        meta << "width=" << frame.Width << " height=" << frame.Height << " images=AO,deferred,SSR,composition\n";
    }
    return result;
}
} // namespace Meta

#include "SceneEnvironment.h"
#include "NativeWork.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace Meta
{
namespace
{
constexpr State Read = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
}
SceneEnvironment::SceneEnvironment(GDX12DeviceResources &r, RenderPipelineCommonData &c) : _resources(r), _common(c)
{
    _environment.Setup(&r, &c);
    _environment.Initialize();
    _cloud.Setup(&r, &c);
    // Resource creation remains an owner-only prerequisite.
}
SceneEnvironment::~SceneEnvironment()
{
    _environment.ClearDenendencies();
    _cloud.ClearDenendencies();
}
void SceneEnvironment::SetClouds(const GDX12CloudParameters &p)
{
    _cloud.SetParameters(p);
    _cloudSelected = p.Enabled;
}
void SceneEnvironment::ResetClouds()
{
    _cloud.Reset();
}
void SceneEnvironment::RestoreViewsAfterBaseline()
{
    _resources.Device->GetCommandQueue()->Flush();
    _environment.PublishAtmosphereViews(_environment._source == SkySource::SkyAtmosphere);
}
bool SceneEnvironment::Ready() const
{
    const auto done = _resources.Device->GetCommandQueue()->GetFence()->GetCompletedValue();
    if (done == UINT64_MAX)
        throw std::runtime_error("Environment completion fence reports device removal");
    return _submitted && done >= _submitted;
}
void SceneEnvironment::Submitted(uint64_t fence)
{
    if (!fence)
        throw std::invalid_argument("Missing environment completion");
    if (!_work.empty())
    {
        _submitted = fence;
        _retained.emplace_back(fence, _work);
    }
}
void SceneEnvironment::Freeze(double delta)
{
    const auto completed = _resources.Device->GetCommandQueue()->GetFence()->GetCompletedValue();
    _retained.erase(std::remove_if(_retained.begin(), _retained.end(),
                                   [completed](const auto &entry) { return entry.first <= completed; }),
                    _retained.end());
    _cloud.Advance(_cloudSelected ? delta : 0);
    _cloudInput = _cloud.CaptureInputs();
    _work.clear();
    Build();
    // A new generation (including newly allocated outputs) cannot inherit the
    // previous generation's completed fence. Unchanged cached outputs keep it.
    if (!_work.empty())
        _submitted = 0;
}
void SceneEnvironment::Build()
{
    auto &e = _environment;
    auto &r = _resources;
    auto state = r.Sky;
    const bool clouds = _cloudSelected && _cloudInput.Enabled;
    const bool cloudChanged =
        clouds != e._cloudActive || (clouds && e._cloudSourceRevision != _cloudInput.SourceRevision);
    const bool lightingChanged = clouds && e._cloudLightingRevision != _cloudInput.LightingRevision;
    e._cloudActive = clouds;
    e._cloudSourceRevision = _cloudInput.SourceRevision;
    e._cloudLightingRevision = _cloudInput.LightingRevision;
    if (clouds && !_cloud._texture->IsInitialized())
        _cloud.CreateResources();
    if (clouds)
    {
        state.Source = SkySource::SkyBox;
        state.Quality = _cloudInput.Quality;
        state.Box.RadianceMultiplier = 1;
        state.Box.RotationRadians = 0;
    }
    bool changed = e._invalidated || cloudChanged || e._revision != state.Revision || e._quality != state.Quality ||
                   e._source != state.Source;
    GDX12SkyConstants constants;
    constants.Mode.x = UINT(state.Source);
    SkyIBLSettings ibl;
    SkyAtmosphereSettings atmosphere;
    if (state.Source == SkySource::SkyBox)
    {
        ValidateSkySettings(state.Box);
        ibl = clouds ? ResolveSkyIBLSettings(state.Box.IBL, state.Quality)
                     : ResolveSkyBoxIBLSettings(state.Box.IBL, state.Quality);
        changed |= e._resolvedIBL.CubeResolution != ibl.CubeResolution ||
                   e._resolvedIBL.FilterSamples != ibl.FilterSamples || e._resolvedIBL.Intensity != ibl.Intensity;
        e._resolvedIBL = ibl;
        constants.Box = {state.Box.RadianceMultiplier, std::sin(state.Box.RotationRadians),
                         std::cos(state.Box.RotationRadians), 0};
        // DDS loading/upload is a separate packet below; no legacy queue submission.
        if (!clouds && (e._boxAsset != state.Box.Cubemap || !e._boxTexture->IsInitialized()))
        {
            r.Device->GetCommandQueue()->Flush();
            const auto path = state.Box.Cubemap.is_absolute()
                                  ? state.Box.Cubemap
                                  : std::filesystem::path(ASSETS_FOLDER) / state.Box.Cubemap;
            const auto *source = Engine::Core::AssetManager::GetInstance().LoadTexture(path);
            if (!source || !source->IsCubeMap() || source->GetArraySize() != 6 || !source->GetWidth() ||
                source->GetWidth() != source->GetHeight() || !source->GetMipLevels() ||
                source->GetSubresourceCount() != 6 * source->GetMipLevels())
                throw std::runtime_error("Environment requires a complete DDS cubemap");
            GDX12TextureDesc desc;
            desc.Format = desc.SRVDesc.Format = FormatToSRGB(source->GetFormat());
            desc.Semantic = ETextureSemantic::Color;
            desc.Width = source->GetWidth();
            desc.Height = source->GetHeight();
            desc.SRV_UAV_Heap = r.SRV_UAV_Heap.get();
            desc.SRVHeapIndex =
                e._boxTexture->IsInitialized()
                    ? e._boxTexture->GetSRV()->HeapIndex
                    : desc.SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
            desc.SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            desc.SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
            desc.SRVDesc.TextureCube.MipLevels = source->GetMipLevels();
            const auto shape =
                CD3DX12_RESOURCE_DESC::Tex2D(desc.Format, desc.Width, desc.Height, 6, UINT16(source->GetMipLevels()));
            desc.ExternalResource =
                r.Device->CreateResource(D3D12_HEAP_TYPE_DEFAULT, shape, D3D12_RESOURCE_STATE_COMMON);
            e._boxTexture->Initialize(desc);
            const UINT count = UINT(source->GetSubresourceCount());
            std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(count);
            std::vector<UINT> rows(count);
            std::vector<UINT64> rowBytes(count);
            UINT64 bytes = 0;
            r.Device->GetDevice()->GetCopyableFootprints(&shape, 0, count, 0, footprints.data(), rows.data(),
                                                         rowBytes.data(), &bytes);
            auto upload = std::make_shared<GDX12Resource>();
            upload->D3DResource = r.Device->CreateResource(D3D12_HEAP_TYPE_UPLOAD, CD3DX12_RESOURCE_DESC::Buffer(bytes),
                                                           D3D12_RESOURCE_STATE_GENERIC_READ);
            void *mapped = nullptr;
            const D3D12_RANGE noRead{};
            ThrowIfFailed(upload->D3DResource->Map(0, &noRead, &mapped));
            for (UINT i = 0; i < count; ++i)
            {
                const auto &data = source->GetSubresources()[i];
                if (data.RowPitch < rowBytes[i] || data.Data.size() < data.RowPitch * (rows[i] - 1) + rowBytes[i])
                    throw std::runtime_error("Incomplete environment subresource");
                for (UINT row = 0; row < rows[i]; ++row)
                    std::memcpy(static_cast<std::byte *>(mapped) + footprints[i].Offset +
                                    UINT64(row) * footprints[i].Footprint.RowPitch,
                                data.Data.data() + UINT64(row) * data.RowPitch, SIZE_T(rowBytes[i]));
            }
            upload->D3DResource->Unmap(0, nullptr);
            auto *target = e._boxTexture->GetResource();
            _work.push_back({"environment DDS upload",
                             {{upload.get(), {Invalid, Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ}},
                              {target, {Invalid, Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST}}},
                             [upload, target, footprints](auto *cmd) {
                                 for (UINT i = 0; i < footprints.size(); ++i)
                                 {
                                     const CD3DX12_TEXTURE_COPY_LOCATION dst(target->D3DResource.Get(), i),
                                         src(upload->D3DResource.Get(), footprints[i]);
                                     cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                                 }
                             }});
            e._boxAsset = state.Box.Cubemap;
            changed = true;
        }
    }
    else if (state.Source == SkySource::SkyAtmosphere)
    {
        atmosphere = ResolveSkyAtmosphereSettings(state.Atmosphere, state.Quality);
        ibl = atmosphere.IBL;
        e.EnsureAtmosphere(atmosphere);
        const auto pack = [](const Vector3 &v, float w) { return XMFLOAT4(v.x, v.y, v.z, w); };
        constants.RayleighHeight = pack(atmosphere.RayleighScatteringPerKm, atmosphere.RayleighScaleHeightKm);
        constants.MieScatteringHeight = pack(atmosphere.MieScatteringPerKm, atmosphere.MieScaleHeightKm);
        constants.MieAbsorptionG = pack(atmosphere.MieAbsorptionPerKm, atmosphere.MieAnisotropy);
        constants.OzoneCenter = pack(atmosphere.OzoneAbsorptionPerKm, atmosphere.OzoneCenterKm);
        constants.GroundWidth = pack(atmosphere.GroundAlbedo, atmosphere.OzoneHalfWidthKm);
        constants.CenterRadius = pack(atmosphere.PlanetCenterCm, atmosphere.PlanetRadiusKm);
        constants.Atmosphere = {atmosphere.PlanetRadiusKm + atmosphere.AtmosphereHeightKm, atmosphere.AerialDistanceKm,
                                atmosphere.SunAngularRadiusRadians, ibl.Intensity};
        constants.SunDirection = pack(atmosphere.DirectionToSun, 0);
        constants.SunIlluminance = pack(atmosphere.SunIlluminanceLux * atmosphere.LuminanceScale, 0);
        Vector3 relative = (_common.CameraView.Invert().Translation() - atmosphere.PlanetCenterCm) * .00001f;
        if (relative.LengthSquared() < 1e-12f)
            throw std::invalid_argument("Atmosphere camera at planet center");
        if (relative.Length() < atmosphere.PlanetRadiusKm + .001f)
        {
            relative.Normalize();
            relative *= atmosphere.PlanetRadiusKm + .001f;
        }
        constants.CameraPosition = pack(relative, 0);
        constants.Samples = {atmosphere.TransmittanceSamples, atmosphere.SkySamples,
                             atmosphere.MultipleScatteringDirections, atmosphere.MultipleScatteringSamples};
        constants.Mode.y = atmosphere.AerialSamples;
        constants.Box.z = float(_common.DownscaledWidth ? _common.DownscaledWidth : _common.WindowWidth);
        constants.Box.w = float(_common.DownscaledHeight ? _common.DownscaledHeight : _common.WindowHeight);
        auto capture = constants;
        capture.CameraPosition = pack((atmosphere.IBLCapturePositionCm - atmosphere.PlanetCenterCm) * .00001f, 0);
        r.FrameConstants[r.CurrFrameConstantsIndex]->SkyCB->CopyData(1, capture);
    }
    const bool discontinuity = changed;
    changed |= lightingChanged;
    auto &frame = *r.FrameConstants[r.CurrFrameConstantsIndex];
    frame.SkyCB->CopyData(0, constants);
    if (changed)
        e.EnsureOutputs(state.Source != SkySource::Off, state.Source == SkySource::Off ? 1 : ibl.CubeResolution);
    if (changed && state.Source != SkySource::SkyAtmosphere)
    {
        e.ReleaseAtmosphere();
        e.PublishAtmosphereViews(false);
    }
    if (state.Source != SkySource::SkyBox && e._boxTexture->IsInitialized())
    {
        r.Device->GetCommandQueue()->Flush();
        e._boxTexture = std::make_unique<GDX12Texture>();
        e._boxAsset.clear();
    }
    auto *heap = r.SRV_UAV_Heap->GetHeap().Get();
    const auto add = [&](const std::string &name, std::vector<std::pair<GDX12Resource *, Access>> accesses,
                         std::function<void(ID3D12GraphicsCommandList *)> record) {
        _work.push_back({name, std::move(accesses), [heap, record](auto *cmd) {
                             cmd->SetDescriptorHeaps(1, &heap);
                             record(cmd);
                         }});
    };
    const auto access = [](GDX12Texture *t, Mode mode, State required) {
        return std::make_pair(static_cast<GDX12Resource *>(t->GetResource()), Access{Invalid, mode, required});
    };
    if (clouds && changed)
    {
        auto *texture = _cloud._texture.get();
        const auto input = _cloudInput;
        auto *root = _cloud._root.get();
        auto *pso = _cloud._pipeline.Get();
        const auto uav = texture->GetUAV()->GPUHandle;
        add("local cloud source", {access(texture, Mode::Write, UnorderedAccess)}, [input, root, pso, uav](auto *cmd) {
            struct Constants
            {
                UINT Size, Octaves, Implementation, Enabled;
                float Scale, Coverage, Softness, Padding;
                Vector3 Horizon;
                float PhaseX;
                Vector3 Zenith;
                float PhaseY;
                Vector3 Cloud;
                UINT Seed;
            };
            const auto &p = input.Parameters;
            const Constants c{256,
                              input.Octaves,
                              UINT(p.Implementation),
                              input.Enabled ? 1u : 0u,
                              p.Scale,
                              p.Coverage,
                              p.Softness,
                              0,
                              p.HorizonColor,
                              input.Phase.x,
                              p.ZenithColor,
                              input.Phase.y,
                              p.CloudColor,
                              p.Seed};
            static_assert(sizeof(c) == 80);
            cmd->SetComputeRootSignature(root->GetRootSignature().Get());
            cmd->SetPipelineState(pso);
            cmd->SetComputeRoot32BitConstants(0, 20, &c, 0);
            cmd->SetComputeRootDescriptorTable(root->GetURootParamIndex(0), uav);
            cmd->Dispatch(32, 32, 6);
        });
    }
    const auto sky = [&](UINT pipeline, UINT constantsIndex, std::vector<std::pair<GDX12Resource *, Access>> accesses,
                         UINT x, UINT y, UINT z, std::array<GDX12Texture *, 4> srvs,
                         std::array<GDX12Descriptor *, 4> uavs) {
        auto *root = e._skyRoot.get();
        auto *pso = e._skyPipelines[pipeline].Get();
        const auto cb = frame.SkyCB->GetElementAddress(constantsIndex),
                   camera = frame.CameraCB->GetElementAddress(_common.ActiveCameraCBufferIndex);
        std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 4> reads{}, writes{};
        for (UINT i = 0; i < 4; ++i)
        {
            reads[i] = srvs[i] ? srvs[i]->GetSRV()->GPUHandle : (i == 3 ? e._nullCube.GPUHandle : e._null2D.GPUHandle);
            if (uavs[i])
                writes[i] = uavs[i]->GPUHandle;
        }
        accesses.push_back({&frame.SkyCB->GetResource(), {Invalid, Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ}});
        accesses.push_back({&frame.CameraCB->GetResource(), {Invalid, Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ}});
        add("atmosphere/source kernel " + std::to_string(pipeline), std::move(accesses),
            [root, pso, cb, camera, reads, writes, x, y, z](auto *cmd) {
                cmd->SetComputeRootSignature(root->GetRootSignature().Get());
                cmd->SetPipelineState(pso);
                cmd->SetComputeRootConstantBufferView(root->GetBRootParamIndex(0), cb);
                cmd->SetComputeRootConstantBufferView(root->GetBRootParamIndex(1), camera);
                for (UINT i = 0; i < 4; ++i)
                {
                    cmd->SetComputeRootDescriptorTable(root->GetTRootParamIndex(i), reads[i]);
                    if (writes[i].ptr)
                        cmd->SetComputeRootDescriptorTable(root->GetURootParamIndex(i), writes[i]);
                }
                cmd->Dispatch(x, y, z);
            });
    };
    if (state.Source == SkySource::SkyAtmosphere)
    {
        auto &l = e._luts;
        if (changed)
        {
            sky(0, 1, {access(l[0].get(), Mode::Write, UnorderedAccess)}, (l[0]->GetWidth() + 7) / 8,
                (l[0]->GetHeight() + 7) / 8, 1, {}, {l[0]->GetUAV()});
            sky(1, 1, {access(l[0].get(), Mode::Read, Read), access(l[1].get(), Mode::Write, UnorderedAccess)},
                (l[1]->GetWidth() + 7) / 8, (l[1]->GetHeight() + 7) / 8, 1, {l[0].get()}, {l[1]->GetUAV()});
            sky(2, 1,
                {access(l[0].get(), Mode::Read, Read), access(l[1].get(), Mode::Read, Read),
                 access(l[2].get(), Mode::Write, UnorderedAccess)},
                (l[2]->GetWidth() + 7) / 8, (l[2]->GetHeight() + 7) / 8, 1, {l[0].get(), l[1].get()}, {l[2]->GetUAV()});
        }
    }
    if (changed && state.Source != SkySource::Off)
    {
        for (UINT mip = 0; mip < e._sourceUAVs.size(); ++mip)
        {
            std::vector<std::pair<GDX12Resource *, Access>> accesses{
                access(e._textures[3].get(), Mode::Write, UnorderedAccess)};
            std::array<GDX12Texture *, 4> inputs{};
            if (state.Source == SkySource::SkyAtmosphere)
            {
                for (UINT i = 0; i < 3; ++i)
                {
                    inputs[i] = e._luts[i].get();
                    accesses.push_back(access(inputs[i], Mode::Read, Read));
                }
            }
            else
            {
                inputs[3] = clouds ? _cloud._texture.get() : e._boxTexture.get();
                accesses.push_back(access(inputs[3], Mode::Read, Read));
            }
            const UINT size = (std::max)(1u, e._textures[3]->GetWidth() >> mip);
            sky(4, state.Source == SkySource::SkyAtmosphere ? 1 : 0, std::move(accesses), (size + 7) / 8,
                (size + 7) / 8, 6, inputs, {nullptr, nullptr, nullptr, e._sourceUAVs[mip].get()});
        }
        const auto filter = [&](UINT pipeline, UINT size, float roughness, GDX12Descriptor *uav, GDX12Texture *target) {
            auto *root = e._filterRoot.get();
            auto *pso = e._filters[pipeline].Get();
            const auto srv = e._textures[3]->GetSRV()->GPUHandle, output = uav->GPUHandle;
            const UINT samples = ibl.FilterSamples;
            const float intensity = ibl.Intensity;
            add("environment filter " + std::to_string(pipeline) + " roughness " + std::to_string(roughness),
                {access(e._textures[3].get(), Mode::Read, Read), access(target, Mode::Write, UnorderedAccess)},
                [root, pso, srv, output, size, roughness, samples, intensity](auto *cmd) {
                    const struct
                    {
                        UINT Size;
                        float Roughness;
                        UINT Samples;
                        float Intensity;
                    } c{size, roughness, samples, intensity};
                    cmd->SetComputeRootSignature(root->GetRootSignature().Get());
                    cmd->SetPipelineState(pso);
                    cmd->SetComputeRoot32BitConstants(0, 4, &c, 0);
                    cmd->SetComputeRootDescriptorTable(root->GetTRootParamIndex(0), srv);
                    cmd->SetComputeRootDescriptorTable(root->GetURootParamIndex(0), output);
                    cmd->Dispatch((size + 7) / 8, (size + 7) / 8, 6);
                });
        };
        filter(0, e._textures[0]->GetWidth(), 0, e._textures[0]->GetUAV(), e._textures[0].get());
        for (UINT mip = 0; mip < e._prefilterMips; ++mip)
            filter(1, e._textures[1]->GetWidth() >> mip, e._prefilterMips > 1 ? mip / 7.f : 0,
                   e._prefilterUAVs[mip].get(), e._textures[1].get());
    }
    else if (changed)
    {
        if (!e._disabledPixel)
        {
            e._disabledPixel = std::make_unique<GDX12UploadBuffer<std::array<uint16_t, 4>>>(r.Device, 1);
            e._disabledPixel->CopyData(0, {0, 0, 0, 0x3c00});
        }
        for (UINT i : {0u, 1u, 3u})
        {
            auto *target = e._textures[i]->GetResource();
            auto *source = e._disabledPixel->GetResource().D3DResource.Get();
            add("disabled environment black " + std::to_string(i),
                {access(e._textures[i].get(), Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST),
                 {&e._disabledPixel->GetResource(), {Invalid, Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ}}},
                [target, source](auto *cmd) {
                    const D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{
                        0, {DXGI_FORMAT_R16G16B16A16_FLOAT, 1, 1, 1, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT}};
                    const CD3DX12_TEXTURE_COPY_LOCATION src(source, fp);
                    for (UINT face = 0; face < 6; ++face)
                    {
                        const CD3DX12_TEXTURE_COPY_LOCATION dst(target->D3DResource.Get(), face);
                        cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                    }
                });
        }
    }
    if (!e._brdfReady)
    {
        auto *root = e._filterRoot.get();
        auto *pso = e._filters[2].Get();
        const auto uav = e._textures[2]->GetUAV()->GPUHandle;
        const UINT size = e._textures[2]->GetWidth();
        add("environment BRDF integration", {access(e._textures[2].get(), Mode::Write, UnorderedAccess)},
            [root, pso, uav, size](auto *cmd) {
                const UINT c[4] = {size, 0, 0, 0};
                cmd->SetComputeRootSignature(root->GetRootSignature().Get());
                cmd->SetPipelineState(pso);
                cmd->SetComputeRoot32BitConstants(0, 4, c, 0);
                cmd->SetComputeRootDescriptorTable(root->GetURootParamIndex(1), uav);
                cmd->Dispatch((size + 7) / 8, (size + 7) / 8, 1);
            });
        e._brdfReady = true;
    }
    if (state.Source == SkySource::SkyAtmosphere)
    {
        auto &l = e._luts;
        sky(2, 0,
            {access(l[0].get(), Mode::Read, Read), access(l[1].get(), Mode::Read, Read),
             access(l[2].get(), Mode::Write, UnorderedAccess)},
            (l[2]->GetWidth() + 7) / 8, (l[2]->GetHeight() + 7) / 8, 1, {l[0].get(), l[1].get()}, {l[2]->GetUAV()});
        sky(3, 0,
            {access(l[0].get(), Mode::Read, Read), access(l[1].get(), Mode::Read, Read),
             access(l[3].get(), Mode::Write, UnorderedAccess), access(l[4].get(), Mode::Write, UnorderedAccess)},
            (atmosphere.AerialWidth + 3) / 4, (atmosphere.AerialHeight + 3) / 4, (atmosphere.AerialDepth + 3) / 4,
            {l[0].get(), l[1].get()}, {nullptr, l[3]->GetUAV(), l[4]->GetUAV()});
    }
    if (discontinuity)
        e.ResetEnvironmentHistory();
    e._revision = state.Revision;
    e._quality = state.Quality;
    e._source = state.Source;
    e._invalidated = false;
}
void SceneEnvironment::Append(Graph &graph, std::vector<ID3D12Resource *> &bindings)
{
    if (graph.Resources.size() != bindings.size())
        throw std::logic_error("Environment binding mismatch");
    _operations.clear();
    for (const auto &work : _work)
    {
        Operation operation{work.Name};
        for (auto [resource, access] : work.Accesses)
        {
            auto *native = resource->D3DResource.Get();
            const auto found = std::find(bindings.begin(), bindings.end(), native);
            access.Resource = Id(found - bindings.begin());
            if (found == bindings.end())
            {
                D3D12_HEAP_PROPERTIES heap{};
                D3D12_HEAP_FLAGS flags{};
                ThrowIfFailed(native->GetHeapProperties(&heap, &flags));
                const bool upload = heap.Type == D3D12_HEAP_TYPE_UPLOAD;
                graph.Resources.push_back({work.Name + " resource " + std::to_string(access.Resource),
                                           upload ? State(D3D12_RESOURCE_STATE_GENERIC_READ) : 0,
                                           upload ? State(D3D12_RESOURCE_STATE_GENERIC_READ) : 0,
                                           (native->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0,
                                           !upload && native->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER,
                                           upload ? Heap::Upload : Heap::Default});
                bindings.push_back(native);
            }
            operation.Accesses.push_back(access);
        }
        _operations.push_back(Id(graph.Operations.size()));
        graph.Operations.push_back(std::move(operation));
    }
}
bool SceneEnvironment::Record(Id operation, ID3D12GraphicsCommandList *cmd) const
{
    const auto found = std::find(_operations.begin(), _operations.end(), operation);
    if (found == _operations.end())
        return false;
    _work[found - _operations.begin()].Record(cmd);
    return true;
}
std::vector<Access> SceneEnvironment::Reads(Graph &graph, std::vector<ID3D12Resource *> &bindings) const
{
    std::vector<Access> accesses;
    for (const auto &texture : _environment._textures)
        accesses.push_back(
            {NativeWork::ResourceFor(graph, bindings, texture->GetResource()->D3DResource.Get(), "environment input"),
             Mode::Read, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE});
    for (const auto &texture : _environment._luts)
        if (texture->IsInitialized())
            accesses.push_back({NativeWork::ResourceFor(graph, bindings, texture->GetResource()->D3DResource.Get(),
                                                        "atmosphere input"),
                                Mode::Read, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE});
    return accesses;
}

} // namespace Meta

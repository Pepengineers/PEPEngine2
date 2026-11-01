#include "Engine.RendererDX12/RenderPasses/GDX12EnvironmentIBLPass.h"
#include "Engine.Core/AssetManager.h"
#include <cstring>
#include <cmath>
#include <stdexcept>

GDX12EnvironmentIBLPass::GDX12EnvironmentIBLPass()
{
    _numOutputs = 4;
    _flags = RENDER_PASS_FLAG_USE_CAMERAS;
    for (auto& texture : _textures)
    {
        texture = std::make_unique<GDX12Texture>();
        _outputs.push_back(texture.get());
    }
    _boxTexture = std::make_unique<GDX12Texture>();
    for (auto& lut : _luts) lut = std::make_unique<GDX12Texture>();
}

void GDX12EnvironmentIBLPass::Initialize()
{
    EnsureOutputs(false, 1);
    CreateTexture(*_textures[2], 128, 128, 1, 1, false);
    D3D12_SHADER_RESOURCE_VIEW_DESC nullCube = {};
    nullCube.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    nullCube.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    nullCube.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    nullCube.TextureCube.MipLevels = 1;
    _nullCube.InitAsSRV(nullptr, &nullCube, _resources->SRV_UAV_Heap.get(),
        _resources->SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength));
    GDX12RootSignatureDesc filter;
    filter.Constants = { 4 };
    filter.NumSingleSRVSlots = 1;
    filter.NumSingleUAVSlots = 2;
    filter.StaticSamplers.emplace_back(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    _filterRoot = std::make_unique<GDX12RootSignature>(_resources->Device, filter);
    GDX12RootSignatureDesc sky;
    sky.NumSingleCBVSlots = 2;
    sky.NumSingleSRVSlots = 4;
    sky.NumSingleUAVSlots = 4;
    sky.StaticSamplers = filter.StaticSamplers;
    _skyRoot = std::make_unique<GDX12RootSignature>(_resources->Device, sky);
    const auto compile = [&](const wchar_t* file, const char* entry, const D3D_SHADER_MACRO* defines,
        GDX12RootSignature* root, ComPtr<ID3D12PipelineState>& pipeline)
    {
        auto shader = GDX12ShaderCompiler::GetInstance().CompileShader(_resources->Device, file, defines, entry, "cs");
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = root->GetRootSignature().Get();
        desc.CS = { shader->GetBufferPointer(), shader->GetBufferSize() };
        ThrowIfFailed(_resources->Device->GetDevice()->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline)));
    };
    const D3D_SHADER_MACRO defines[] = { { "GLOBAL_SKY_FILTER", "1" }, { nullptr, nullptr } };
    const char* filters[] = { "IrradianceCS", "PrefilterCS", "IntegrateBRDFCS" };
    for (UINT i = 0; i < 3; ++i)
        compile(SHADERS_FOLDER "EnvironmentIBL.hlsl", filters[i], defines, _filterRoot.get(), _filters[i]);
    const char* entries[] = { "TransmittanceCS", "MultipleScatteringCS", "SkyViewCS", "AerialCS", "SourceCubeCS" };
    for (UINT i = 0; i < 5; ++i)
        compile(SHADERS_FOLDER "SkyAtmosphere.hlsl", entries[i], nullptr, _skyRoot.get(), _skyPipelines[i]);
}

void GDX12EnvironmentIBLPass::Invalidate() { _invalidated = true; }

void GDX12EnvironmentIBLPass::CreateTexture(GDX12Texture& texture, UINT width, UINT height, UINT depth, UINT mips, bool cube)
{
    // Every replacement and descriptor rewrite is preceded by a queue drain in Ensure*.
    const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    const auto resource = depth > 1 && !cube ?
        CD3DX12_RESOURCE_DESC::Tex3D(DXGI_FORMAT_R16G16B16A16_FLOAT, width, height, static_cast<UINT16>(depth), 1,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) :
        CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16G16B16A16_FLOAT, width, height, cube ? 6 : 1,
            static_cast<UINT16>(mips), 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    GDX12TextureDesc desc;
    ThrowIfFailed(_resources->Device->GetDevice()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
        &resource, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&desc.ExternalResource)));
    desc.Width = width;
    desc.Height = height;
    desc.Format = desc.SRVDesc.Format = desc.UAVDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.CreateUAV = true;
    desc.SRV_UAV_Heap = _resources->SRV_UAV_Heap.get();
    desc.SRVHeapIndex = texture.IsInitialized() ? texture.GetSRV()->HeapIndex :
        desc.SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
    desc.UAVHeapIndex = texture.IsInitialized() ? texture.GetUAV()->HeapIndex :
        desc.SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
    desc.SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (cube)
    {
        desc.SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        desc.SRVDesc.TextureCube.MipLevels = mips;
        desc.UAVDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
        desc.UAVDesc.Texture2DArray.ArraySize = 6;
    }
    else if (depth > 1)
    {
        desc.SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        desc.SRVDesc.Texture3D.MipLevels = 1;
        desc.UAVDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        desc.UAVDesc.Texture3D.WSize = depth;
    }
    else
    {
        desc.SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        desc.SRVDesc.Texture2D.MipLevels = mips;
        desc.UAVDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    }
    texture.Initialize(desc);
}

void GDX12EnvironmentIBLPass::EnsureOutputs(bool enabled, UINT cubeSize)
{
    const UINT mips = enabled ? 8 : 1;
    if (_prefilterMips == mips && _textures[3]->GetWidth() == cubeSize) return;
    _resources->Device->GetCommandQueue()->Flush();
    CreateTexture(*_textures[0], enabled ? 32 : 1, enabled ? 32 : 1, 1, 1, true);
    CreateTexture(*_textures[1], enabled ? 128 : 1, enabled ? 128 : 1, 1, mips, true);
    UINT sourceMips = 1;
    for (UINT size = cubeSize; size > 1; size >>= 1) ++sourceMips;
    CreateTexture(*_textures[3], cubeSize, cubeSize, 1, sourceMips, true);
    _sourceUAVs.resize(sourceMips);
    for (UINT mip = 0; mip < sourceMips; ++mip)
    {
        if (!_sourceUAVs[mip]) _sourceUAVs[mip] = std::make_unique<GDX12Descriptor>();
        auto& uav = *_sourceUAVs[mip];
        D3D12_UNORDERED_ACCESS_VIEW_DESC view = {};
        view.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
        view.Texture2DArray.ArraySize = 6;
        view.Texture2DArray.MipSlice = mip;
        const UINT index = uav.HeapIndex == UINT_MAX ? _resources->SRV_UAV_Heap->GetAvailableIndex(
            TextureResources_StartIndex, TextureResources_RangeLength) : uav.HeapIndex;
        uav.InitAsUAV(_textures[3]->GetResource()->D3DResource.Get(), nullptr, &view, _resources->SRV_UAV_Heap.get(), index);
    }
    _prefilterMips = mips;
    for (UINT mip = 0; mip < mips; ++mip)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC view = {};
        view.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
        view.Texture2DArray.ArraySize = 6;
        view.Texture2DArray.MipSlice = mip;
        if (!_prefilterUAVs[mip]) _prefilterUAVs[mip] = std::make_unique<GDX12Descriptor>();
        auto& uav = *_prefilterUAVs[mip];
        const UINT index = uav.HeapIndex != UINT_MAX ? uav.HeapIndex :
            _resources->SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
        uav.InitAsUAV(_textures[1]->GetResource()->D3DResource.Get(), nullptr, &view, _resources->SRV_UAV_Heap.get(), index);
    }
}

void GDX12EnvironmentIBLPass::PublishAtmosphereViews(bool enabled)
{
    GDX12Descriptor* destinations[] = { &_resources->SkyTransmittanceSRV, &_resources->SkyViewSRV,
        &_resources->SkyAerialRadianceSRV, &_resources->SkyAerialTransmittanceSRV };
    const UINT indices[] = { 0, 2, 3, 4 };
    for (UINT i = 0; i < 4; ++i)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC view = {};
        view.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.ViewDimension = i < 2 ? D3D12_SRV_DIMENSION_TEXTURE2D : D3D12_SRV_DIMENSION_TEXTURE3D;
        if (i < 2) view.Texture2D.MipLevels = 1;
        else view.Texture3D.MipLevels = 1;
        destinations[i]->InitAsSRV(enabled ? _luts[indices[i]]->GetResource()->D3DResource.Get() : nullptr,
            &view, _resources->SRV_UAV_Heap.get(), destinations[i]->HeapIndex);
    }
}

void GDX12EnvironmentIBLPass::EnsureAtmosphere(const SkyAtmosphereSettings& s)
{
    const UINT sizes[5][3] = { { s.TransmittanceWidth, s.TransmittanceHeight, 1 },
        { s.MultipleScatteringSize, s.MultipleScatteringSize, 1 }, { s.SkyViewWidth, s.SkyViewHeight, 1 },
        { s.AerialWidth, s.AerialHeight, s.AerialDepth }, { s.AerialWidth, s.AerialHeight, s.AerialDepth } };
    bool changed = false;
    for (UINT i = 0; i < 5; ++i)
        changed |= !_luts[i]->IsInitialized() || _luts[i]->GetWidth() != sizes[i][0] || _luts[i]->GetHeight() != sizes[i][1] ||
            _luts[i]->GetResource()->D3DResource->GetDesc().DepthOrArraySize != sizes[i][2];
    if (!changed) return;
    _resources->Device->GetCommandQueue()->Flush();
    for (UINT i = 0; i < 5; ++i) CreateTexture(*_luts[i], sizes[i][0], sizes[i][1], sizes[i][2], 1, false);
    PublishAtmosphereViews(true);
}

void GDX12EnvironmentIBLPass::BindSky(GDX12CommandList* commands, UINT constantIndex)
{
    auto& frame = _resources->FrameConstants[_resources->CurrFrameConstantsIndex];
    commands->SetComputeRootSignature(_skyRoot.get());
    commands->SetComputeRootConstantBufferView(0, frame->SkyCB->GetElementAddress(constantIndex));
    commands->SetComputeSRV(0, _resources->SkyTransmittanceSRV.GPUHandle);
    commands->SetComputeSRV(1, _resources->SkyTransmittanceSRV.GPUHandle);
    commands->SetComputeSRV(2, _resources->SkyViewSRV.GPUHandle);
    commands->SetComputeSRV(3, _nullCube.GPUHandle);
    if (_commonData->ActiveCameraCBufferIndex != UINT_MAX)
        commands->SetComputeRootConstantBufferView(1, frame->CameraCB->GetElementAddress(_commonData->ActiveCameraCBufferIndex));
}

void GDX12EnvironmentIBLPass::DispatchLUT(GDX12CommandList* commands, UINT pipeline, UINT index)
{
    commands->SetPipelineState(_skyPipelines[pipeline]);
    auto* texture = _luts[index].get();
    commands->ResourceBarrier({ texture->GetResource()->GetUnorderedAccessBarrier() });
    commands->SetComputeUAV(0, texture->GetUAV()->GPUHandle);
    commands->Dispatch((texture->GetWidth() + 7) / 8, (texture->GetHeight() + 7) / 8, 1);
    commands->ResourceBarrier({ texture->GetResource()->GetSRVBarrier() });
}

void GDX12EnvironmentIBLPass::Filter(GDX12CommandList* commands, UINT samples, float intensity)
{
    commands->SetComputeRootSignature(_filterRoot.get());
    commands->SetComputeSRV(0, _textures[3]->GetSRV()->GPUHandle);
    for (UINT i = 0; i < 3; ++i) commands->ResourceBarrier({ _textures[i]->GetResource()->GetUnorderedAccessBarrier() });
    const auto dispatch = [&](UINT pipeline, UINT size, float roughness, D3D12_GPU_DESCRIPTOR_HANDLE uav, UINT slot, UINT faces)
    {
        struct { UINT Size; float Roughness; UINT Samples; float Intensity; } parameters = { size, roughness, samples, intensity };
        commands->SetPipelineState(_filters[pipeline]);
        commands->GetCommandList()->SetComputeRoot32BitConstants(0, 4, &parameters, 0);
        commands->SetComputeUAV(slot, uav);
        commands->Dispatch((size + 7) / 8, (size + 7) / 8, faces);
    };
    dispatch(0, _textures[0]->GetWidth(), 0, _textures[0]->GetUAV()->GPUHandle, 0, 6);
    for (UINT mip = 0; mip < _prefilterMips; ++mip)
        dispatch(1, _textures[1]->GetWidth() >> mip, _prefilterMips > 1 ? mip / 7.0f : 0,
            _prefilterUAVs[mip]->GPUHandle, 0, 6);
    if (!_brdfReady)
    {
        dispatch(2, 128, 0, _textures[2]->GetUAV()->GPUHandle, 1, 1);
        _brdfReady = true;
    }
    for (UINT i = 0; i < 3; ++i) commands->ResourceBarrier({ _textures[i]->GetResource()->GetSRVBarrier() });
}

void GDX12EnvironmentIBLPass::Execute(GDX12CommandList* commands)
{
    const auto& state = _resources->Sky;
    const bool changed = _invalidated || _revision != state.Revision || _quality != state.Quality || _source != state.Source;
    auto& frame = _resources->FrameConstants[_resources->CurrFrameConstantsIndex];
    GDX12SkyConstants constants;
    constants.Mode.x = static_cast<UINT>(state.Source);
    SkyIBLSettings ibl;
    SkyAtmosphereSettings atmosphere;
    if (state.Source == SkySource::SkyBox)
    {
        ValidateSkySettings(state.Box);
        ibl = ResolveSkyIBLSettings(state.Box.IBL, state.Quality);
        constants.Box = { state.Box.RadianceMultiplier, std::sin(state.Box.RotationRadians), std::cos(state.Box.RotationRadians), 0 };
        if (_boxAsset != state.Box.Cubemap || !_boxTexture->IsInitialized())
        {
            _resources->Device->GetCommandQueue()->Flush();
            LoadEnvironment(state.Box.Cubemap);
            _boxAsset = state.Box.Cubemap;
        }
    }
    else if (state.Source == SkySource::SkyAtmosphere)
    {
        atmosphere = ResolveSkyAtmosphereSettings(state.Atmosphere, state.Quality);
        ibl = atmosphere.IBL;
        EnsureAtmosphere(atmosphere);
        const auto pack = [](const Vector3& value, float w) { return XMFLOAT4(value.x, value.y, value.z, w); };
        constants.RayleighHeight = pack(atmosphere.RayleighScatteringPerKm, atmosphere.RayleighScaleHeightKm);
        constants.MieScatteringHeight = pack(atmosphere.MieScatteringPerKm, atmosphere.MieScaleHeightKm);
        constants.MieAbsorptionG = pack(atmosphere.MieAbsorptionPerKm, atmosphere.MieAnisotropy);
        constants.OzoneCenter = pack(atmosphere.OzoneAbsorptionPerKm, atmosphere.OzoneCenterKm);
        constants.GroundWidth = pack(atmosphere.GroundAlbedo, atmosphere.OzoneHalfWidthKm);
        constants.CenterRadius = pack(atmosphere.PlanetCenterCm, atmosphere.PlanetRadiusKm);
        constants.Atmosphere = { atmosphere.PlanetRadiusKm + atmosphere.AtmosphereHeightKm,
            atmosphere.AerialDistanceKm, atmosphere.SunAngularRadiusRadians, ibl.Intensity };
        constants.SunDirection = pack(atmosphere.DirectionToSun, 0);
        constants.SunIlluminance = pack(atmosphere.SunIlluminanceLux * atmosphere.LuminanceScale, 0);
        const Vector3 camera = _commonData->CameraView.Invert().Translation();
        Vector3 relative = (camera - atmosphere.PlanetCenterCm) * 0.00001f;
        if (relative.LengthSquared() < 1e-12f) throw std::invalid_argument("Atmosphere camera is at planet center");
        if (relative.Length() < atmosphere.PlanetRadiusKm + 0.001f)
        {
            relative.Normalize();
            relative *= atmosphere.PlanetRadiusKm + 0.001f;
        }
        constants.CameraPosition = pack(relative, 0);
        constants.Samples = { atmosphere.TransmittanceSamples, atmosphere.SkySamples,
            atmosphere.MultipleScatteringDirections, atmosphere.MultipleScatteringSamples };
        constants.Mode.y = atmosphere.AerialSamples;
        constants.Box.z = static_cast<float>(_commonData->DownscaledWidth ? _commonData->DownscaledWidth : _commonData->WindowWidth);
        constants.Box.w = static_cast<float>(_commonData->DownscaledHeight ? _commonData->DownscaledHeight : _commonData->WindowHeight);
        auto capture = constants;
        capture.CameraPosition = pack((atmosphere.IBLCapturePositionCm - atmosphere.PlanetCenterCm) * 0.00001f, 0);
        frame->SkyCB->CopyData(1, capture);
    }
    frame->SkyCB->CopyData(0, constants);
    if (changed) EnsureOutputs(state.Source != SkySource::Off, state.Source == SkySource::Off ? 1 : ibl.CubeResolution);
    if (changed && state.Source != SkySource::SkyAtmosphere && _luts[0]->IsInitialized())
    {
        _resources->Device->GetCommandQueue()->Flush();
        PublishAtmosphereViews(false);
        for (auto& lut : _luts) lut = std::make_unique<GDX12Texture>();
    }
    if (state.Source != SkySource::SkyBox && _boxTexture->IsInitialized())
    {
        _resources->Device->GetCommandQueue()->Flush();
        _boxTexture = std::make_unique<GDX12Texture>();
        _boxAsset.clear();
    }
    if (!changed && state.Source != SkySource::SkyAtmosphere) return;
    commands->BeginPixEvent("Sky source and atmosphere LUTs", Colors::CornflowerBlue);
    commands->SetDescriptorHeaps({ _resources->SRV_UAV_Heap.get() });
    BindSky(commands, state.Source == SkySource::SkyAtmosphere ? 1 : 0);
    if (state.Source == SkySource::SkyAtmosphere)
    {
        if (changed)
        {
            DispatchLUT(commands, 0, 0);
            commands->SetComputeSRV(0, _luts[0]->GetSRV()->GPUHandle);
            DispatchLUT(commands, 1, 1);
        }
        commands->SetComputeSRV(0, _luts[0]->GetSRV()->GPUHandle);
        commands->SetComputeSRV(1, _luts[1]->GetSRV()->GPUHandle);
        if (changed) DispatchLUT(commands, 2, 2);
        commands->SetComputeSRV(2, _luts[2]->GetSRV()->GPUHandle);
    }
    else if (state.Source == SkySource::SkyBox)
        commands->SetComputeSRV(3, _boxTexture->GetSRV()->GPUHandle);
    if (changed)
    {
        commands->ResourceBarrier({ _textures[3]->GetResource()->GetUnorderedAccessBarrier() });
        commands->SetPipelineState(_skyPipelines[4]);
        for (UINT mip = 0; mip < _sourceUAVs.size(); ++mip)
        {
            const UINT size = (std::max)(1u, _textures[3]->GetWidth() >> mip);
            commands->SetComputeUAV(3, _sourceUAVs[mip]->GPUHandle);
            commands->Dispatch((size + 7) / 8, (size + 7) / 8, 6);
        }
        commands->ResourceBarrier({ _textures[3]->GetResource()->GetSRVBarrier() });
        Filter(commands, state.Source == SkySource::Off ? 16 : ibl.FilterSamples, state.Source == SkySource::Off ? 0 : ibl.Intensity);
        ++_resources->ReflectionEnvironmentRevision;
        // Components/quality/source are discontinuities. Continuous camera motion keeps its history.
        _commonData->ResetHistory = true;
        _commonData->ResetUpscalerHistory = true;
        _commonData->RequestHistoryReset = true;
    }
    if (state.Source == SkySource::SkyAtmosphere)
    {
        BindSky(commands, 0);
        commands->SetComputeSRV(0, _luts[0]->GetSRV()->GPUHandle);
        commands->SetComputeSRV(1, _luts[1]->GetSRV()->GPUHandle);
        DispatchLUT(commands, 2, 2);
        if (_commonData->ActiveCameraCBufferIndex != UINT_MAX)
        {
            commands->ResourceBarrier({ _luts[3]->GetResource()->GetUnorderedAccessBarrier(), _luts[4]->GetResource()->GetUnorderedAccessBarrier() });
            commands->SetComputeUAV(1, _luts[3]->GetUAV()->GPUHandle);
            commands->SetComputeUAV(2, _luts[4]->GetUAV()->GPUHandle);
            commands->SetPipelineState(_skyPipelines[3]);
            commands->Dispatch((atmosphere.AerialWidth + 3) / 4, (atmosphere.AerialHeight + 3) / 4, (atmosphere.AerialDepth + 3) / 4);
            commands->ResourceBarrier({ _luts[3]->GetResource()->GetSRVBarrier(), _luts[4]->GetResource()->GetSRVBarrier() });
        }
    }
    commands->EndPixEvent();
    _revision = state.Revision;
    _quality = state.Quality;
    _source = state.Source;
    _invalidated = false;
}

void GDX12EnvironmentIBLPass::ClearDenendencies()
{
    GDX12RenderPass::ClearDenendencies();
    for (auto& texture : _textures) texture.reset();
    for (auto& texture : _luts) texture.reset();
    for (auto& uav : _prefilterUAVs) uav.reset();
    for (auto& pipeline : _filters) pipeline.Reset();
    for (auto& pipeline : _skyPipelines) pipeline.Reset();
    _sourceUAVs.clear();
    _boxTexture.reset();
    _filterRoot.reset();
    _skyRoot.reset();
}

    void GDX12EnvironmentIBLPass::LoadEnvironment(const std::filesystem::path& asset)
    {
        const auto path = asset.is_absolute() ? asset : std::filesystem::path(ASSETS_FOLDER) / asset;
        const auto* source = Engine::Core::AssetManager::GetInstance().LoadTexture(path);
        if (!source || !source->IsCubeMap() || source->GetArraySize() != 6 ||
            source->GetWidth() == 0 || source->GetWidth() != source->GetHeight() ||
            source->GetMipLevels() == 0 || source->GetMipLevels() > D3D12_REQ_MIP_LEVELS ||
            source->GetSubresourceCount() != 6u * source->GetMipLevels())
            throw std::runtime_error("Environment IBL requires a complete DDS cubemap: " + path.string());

        GDX12TextureDesc desc;
        desc.Format = desc.SRVDesc.Format = FormatToSRGB(source->GetFormat());
        desc.Semantic = ETextureSemantic::Color;
        desc.Width = source->GetWidth();
        desc.Height = source->GetHeight();
        desc.SRV_UAV_Heap = _resources->SRV_UAV_Heap.get();
        desc.SRVHeapIndex = _boxTexture->IsInitialized() ? _boxTexture->GetSRV()->HeapIndex :
            desc.SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
        desc.SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        desc.SRVDesc.TextureCube.MipLevels = source->GetMipLevels();
        const auto textureDesc = CD3DX12_RESOURCE_DESC::Tex2D(desc.Format, desc.Width, desc.Height,
            6, static_cast<UINT16>(source->GetMipLevels()));
        const auto defaultHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        ThrowIfFailed(_resources->Device->GetDevice()->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE,
            &textureDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&desc.ExternalResource)));
        _boxTexture->Initialize(desc);

        const UINT count = static_cast<UINT>(source->GetSubresourceCount());
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(count);
        std::vector<UINT> rows(count);
        std::vector<UINT64> rowBytes(count);
        UINT64 uploadBytes = 0;
        _resources->Device->GetDevice()->GetCopyableFootprints(&textureDesc, 0, count, 0,
            footprints.data(), rows.data(), rowBytes.data(), &uploadBytes);
        for (UINT i = 0; i < count; ++i)
        {
            const auto& data = source->GetSubresources()[i];
            if (data.RowPitch < rowBytes[i] || data.Data.size() < data.RowPitch * (rows[i] - 1) + rowBytes[i])
                throw std::runtime_error("Environment cubemap has incomplete subresource data: " + path.string());
        }
        const auto uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        const auto uploadDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadBytes);
        ComPtr<ID3D12Resource> upload;
        ThrowIfFailed(_resources->Device->GetDevice()->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE,
            &uploadDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)));
        void* mapped = nullptr;
        const D3D12_RANGE noRead = { 0, 0 };
        ThrowIfFailed(upload->Map(0, &noRead, &mapped));
        for (UINT i = 0; i < count; ++i)
        {
            const auto& data = source->GetSubresources()[i];
            auto* destination = static_cast<std::byte*>(mapped) + footprints[i].Offset;
            for (UINT row = 0; row < rows[i]; ++row)
                std::memcpy(destination + row * footprints[i].Footprint.RowPitch,
                    data.Data.data() + row * data.RowPitch, static_cast<size_t>(rowBytes[i]));
        }
        upload->Unmap(0, nullptr);
        auto* queue = _resources->Device->GetCommandQueue();
        auto* commands = queue->GetCommandList();
        commands->ResourceBarrier({ _boxTexture->GetResource()->GetCopyDestBarrier() });
        for (UINT i = 0; i < count; ++i)
        {
            const CD3DX12_TEXTURE_COPY_LOCATION destination(desc.ExternalResource.Get(), i);
            const CD3DX12_TEXTURE_COPY_LOCATION sourceLocation(upload.Get(), footprints[i]);
            commands->GetCommandList()->CopyTextureRegion(&destination, 0, 0, 0, &sourceLocation, nullptr);
        }
        commands->ResourceBarrier({ _boxTexture->GetResource()->GetSRVBarrier() });
        queue->ExecuteCommandList(commands);
        queue->Flush();
    }


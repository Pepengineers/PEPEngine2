#include "Engine.RendererDX12/RenderPasses/GDX12EnvironmentIBLPass.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include "Engine.Core/AssetManager.h"
#include <cstring>
#include <cmath>
#include <stdexcept>
#include <algorithm>

namespace
{
    constexpr UINT DiffuseSize = 32, SpecularSize = 128, SpecularMips = 8;
}

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
    InitializeKernels();
}

void GDX12EnvironmentIBLPass::InitializeKernels()
{
    D3D12_SHADER_RESOURCE_VIEW_DESC nullCube = {};
    nullCube.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    nullCube.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    nullCube.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    nullCube.TextureCube.MipLevels = 1;
    _nullCube.InitAsSRV(nullptr, &nullCube, _resources->SRV_UAV_Heap.get(),
        _resources->SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength));
    auto null2D = nullCube;
    null2D.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    null2D.Texture2D = {};
    null2D.Texture2D.MipLevels = 1;
    _null2D.InitAsSRV(nullptr, &null2D, _resources->SRV_UAV_Heap.get(),
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
    const D3D_SHADER_MACRO defines[] = { { "GLOBAL_SKY_FILTER", "1" }, { nullptr, nullptr } };
    const char* filters[] = { "IrradianceCS", "PrefilterCS", "IntegrateBRDFCS" };
    for (UINT i = 0; i < 3u; ++i)
        _filters[i] = GDX12RenderHelpers::CreateComputePipeline(_resources->Device, _filterRoot.get(),
            SHADERS_FOLDER "EnvironmentIBL.hlsl", filters[i], defines);
    const char* entries[] = { "TransmittanceCS", "MultipleScatteringCS", "SkyViewCS", "AerialCS", "SourceCubeCS" };
    for (UINT i = 0; i < 5; ++i)
        _skyPipelines[i] = GDX12RenderHelpers::CreateComputePipeline(_resources->Device, _skyRoot.get(),
            SHADERS_FOLDER "SkyAtmosphere.hlsl", entries[i]);
}

void GDX12EnvironmentIBLPass::Invalidate()
{
    _invalidated = true;
}

void GDX12EnvironmentIBLPass::Resize()
{
    if (_cloudActive) _invalidated = true;
}

void GDX12EnvironmentIBLPass::CreateTexture(GDX12Texture& texture, const UINT width, const UINT height, const UINT depth, const UINT mips, const bool cube)
{
    // Every replacement and descriptor rewrite is preceded by a queue drain in Ensure*.
    const auto resource = depth > 1 && !cube ?
        CD3DX12_RESOURCE_DESC::Tex3D(DXGI_FORMAT_R16G16B16A16_FLOAT, width, height, static_cast<UINT16>(depth), 1,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) :
        CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16G16B16A16_FLOAT, width, height, cube ? 6 : 1,
            static_cast<UINT16>(mips), 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    GDX12TextureDesc desc;
    desc.ExternalResource = _resources->Device->CreateResource(D3D12_HEAP_TYPE_DEFAULT, resource,
        D3D12_RESOURCE_STATE_COMMON, nullptr);
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
    RetireResources();
    AllocateOutputs(enabled, cubeSize);
}

void GDX12EnvironmentIBLPass::AllocateOutputs(const bool enabled, const UINT cubeSize)
{
    const UINT mips = enabled ? SpecularMips : 1;
    CreateTexture(*_textures[0], enabled ? DiffuseSize : 1, enabled ? DiffuseSize : 1, 1, 1, true);
    CreateTexture(*_textures[1], enabled ? SpecularSize : 1, enabled ? SpecularSize : 1, 1, mips, true);
    const UINT sourceMips = GDX12RenderHelpers::MipCount(cubeSize);
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
    std::array<bool, 5> changed{};
    for (UINT i = 0; i < 5; ++i)
        changed[i] = !_luts[i]->IsInitialized() || _luts[i]->GetWidth() != sizes[i][0] || _luts[i]->GetHeight() != sizes[i][1] ||
            _luts[i]->GetResource()->D3DResource->GetDesc().DepthOrArraySize != sizes[i][2];
    if (std::none_of(changed.begin(), changed.end(), [](bool value) { return value; })) return;
    RetireResources();
    for (UINT i = 0; i < 5; ++i)
        if (changed[i]) CreateTexture(*_luts[i], sizes[i][0], sizes[i][1], sizes[i][2], 1, false);
    PublishAtmosphereViews(true);
}

void GDX12EnvironmentIBLPass::ResetEnvironmentHistory()
{
    ++_resources->ReflectionEnvironmentRevision;
    _commonData->ResetHistory = true;
    _commonData->ResetUpscalerHistory = true;
    _commonData->RequestHistoryReset = true;
}

void GDX12EnvironmentIBLPass::ReleaseAtmosphere()
{
    if (!_luts[0]->IsInitialized()) return;
    RetireResources();
    PublishAtmosphereViews(false);
    for (auto& lut : _luts) lut = std::make_unique<GDX12Texture>();
}

void GDX12EnvironmentIBLPass::RetireResources()
{
    if (_retire) _retire();
    else _resources->Device->GetCommandQueue()->Flush();
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
    _disabledPixel.reset();
    _filterRoot.reset();
    _skyRoot.reset();
}

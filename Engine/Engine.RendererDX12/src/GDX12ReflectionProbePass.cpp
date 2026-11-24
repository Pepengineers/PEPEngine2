#include "Engine.RendererDX12/RenderPasses/GDX12ReflectionProbePass.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <stdexcept>

namespace
{
    UINT64 AllocationBytes(GDX12Device* device, GDX12Texture* texture)
    {
        if (!texture || !texture->IsInitialized()) return 0;
        const auto desc = texture->GetResource()->D3DResource->GetDesc();
        return device->GetDevice()->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
    }

    GDX12CameraConstants FaceCamera(const GDX12ReflectionProbe& probe, const UINT face, const UINT size)
    {
        const Vector3 directions[] = { Vector3::UnitX, -Vector3::UnitX, Vector3::UnitY,
            -Vector3::UnitY, Vector3::UnitZ, -Vector3::UnitZ };
        const Vector3 up[] = { Vector3::UnitY, Vector3::UnitY, -Vector3::UnitZ,
            Vector3::UnitZ, Vector3::UnitY, Vector3::UnitY };
        // RH view with a reflected projection X keeps D3D cubemap face orientation.
        const Matrix view = Matrix::CreateLookAt(probe.Position, probe.Position + directions[face], up[face]);
        Matrix projection = Matrix::CreatePerspectiveFieldOfView(XM_PIDIV2, 1, probe.NearPlane, probe.FarPlane);
        projection._11 = -projection._11;
        const float range = probe.NearPlane / (probe.FarPlane - probe.NearPlane);
        projection._33 = range;
        projection._43 = range * probe.FarPlane;
        const Matrix viewProjection = view * projection;
        GDX12CameraConstants result = {};
        XMStoreFloat4x4(&result.ViewProj, viewProjection.Transpose());
        result.ViewProjNoJitter = result.PrevViewProjNoJitter = result.ViewProj;
        XMStoreFloat4x4(&result.View, view.Transpose());
        XMStoreFloat4x4(&result.InvViewProj, viewProjection.Invert().Transpose());
        result.CameraLocation = probe.Position;
        result.NearPlane = probe.NearPlane;
        result.FarPlane = probe.FarPlane;
        result._pad1 = static_cast<float>(size);
        return result;
    }

    GDX12ShadowConstants ProbeSunShadow(const GDX12DeviceResources& resources, UINT resolution)
    {
        Vector3 direction(resources.Lighting.LightDirection);
        if (direction.LengthSquared() < 1e-8f) direction = -Vector3::UnitY;
        direction.Normalize();
        const auto& bounds = resources.ReflectionSceneBounds;
        const Vector3 center(bounds.Center);
        const float radius = (std::max)(Vector3(bounds.Extents).Length(), 1.0f);
        const Vector3 up = std::abs(direction.y) > 0.95f ? Vector3::UnitZ : Vector3::UnitY;
        const Matrix view = Matrix::CreateLookAt(center - direction * (2 * radius + 1), center, up);
        XMFLOAT3 corners[8];
        bounds.GetCorners(corners);
        Vector3 minimum(FLT_MAX), maximum(-FLT_MAX);
        for (const auto& corner : corners)
        {
            const Vector3 position = Vector3::Transform(Vector3(corner), view);
            minimum = Vector3::Min(minimum, position);
            maximum = Vector3::Max(maximum, position);
        }
        const Matrix projection = Matrix::CreateOrthographicOffCenter(minimum.x - 1, maximum.x + 1,
            minimum.y - 1, maximum.y + 1, (std::max)(0.01f, -maximum.z - 1), -minimum.z + 1);
        GDX12ShadowConstants result;
        result.CascadeCount = 1;
        XMStoreFloat4x4(&result.ViewProj[0], (view * projection).Transpose());
        result.TexelWorldSize.x = (std::max)(maximum.x - minimum.x, maximum.y - minimum.y) / resolution;
        return result;
    }
}

GDX12ReflectionProbePass::GDX12ReflectionProbePass()
{
    _flags = RENDER_PASS_FLAG_USE_GEOMETRY | RENDER_PASS_FLAG_USE_MATERIALS | RENDER_PASS_FLAG_USE_INSTANCES;
    _numInputs = 4;
    _numOutputs = 1;
    _outputs.push_back(&_published);
}

void GDX12ReflectionProbePass::Initialize()
{
    GDX12RootSignatureDesc capture;
    capture.InstancedGeometry = true;
    capture.IndexedLocalShadows = true;
    capture.Constants.push_back(1);
    capture.NumSingleCBVSlots = 4;
    capture.NumSingleSRVSlots = 11;
    capture.SRVRanges.emplace_back(Texture2D_RangeLength);
    capture.StaticSamplers = GetStaticSamplers();
    capture.StaticSamplers.emplace_back(7, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    CD3DX12_STATIC_SAMPLER_DESC sampler(6, D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT);
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    capture.StaticSamplers.push_back(sampler);
    _captureRoot = std::make_unique<GDX12RootSignature>(_resources->Device, capture);
    GDX12RootSignatureDesc cull;
    cull.Constants.push_back(2);
    cull.NumSingleCBVSlots = 2;
    cull.NumSingleSRVSlots = 3;
    cull.NumSingleUAVSlots = 2;
    _cullRoot = std::make_unique<GDX12RootSignature>(_resources->Device, cull);
    GDX12RootSignatureDesc filter;
    filter.Constants.push_back(3);
    filter.NumSingleSRVSlots = 1;
    filter.NumSingleUAVSlots = 1;
    filter.StaticSamplers.emplace_back(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    _filterRoot = std::make_unique<GDX12RootSignature>(_resources->Device, filter);
    auto& compiler = GDX12ShaderCompiler::GetInstance();
    const auto graphics = [&](const char* vsEntry, const char* psEntry, bool shadow, bool sky,
        ComPtr<ID3D12PipelineState>& pipeline)
    {
        auto vs = compiler.CompileShader(_resources->Device, SHADERS_FOLDER "ReflectionCapture.hlsl", nullptr, vsEntry, "vs");
        auto ps = compiler.CompileShader(_resources->Device, SHADERS_FOLDER "ReflectionCapture.hlsl", nullptr, psEntry, "ps");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = _captureRoot->GetRootSignature().Get();
        if (!sky) desc.InputLayout = { _resources->InputLayouts["Default"].data(),
            static_cast<UINT>(_resources->InputLayouts["Default"].size()) };
        desc.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
        desc.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
        desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        // The cube projection reverses X; reverse winding with it.
        desc.RasterizerState.FrontCounterClockwise = shadow ? TRUE : FALSE;
        if (sky) desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        if (shadow)
        {
            desc.RasterizerState.DepthBias = 250;
            desc.RasterizerState.SlopeScaledDepthBias = 1.5f;
        }
        desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        desc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        desc.DepthStencilState.DepthFunc = shadow ? D3D12_COMPARISON_FUNC_LESS_EQUAL : D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        if (sky) desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        desc.SampleMask = UINT_MAX;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = shadow ? 0 : 1;
        desc.RTVFormats[0] = shadow ? DXGI_FORMAT_UNKNOWN : DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        desc.SampleDesc.Count = 1;
        ThrowIfFailed(_resources->Device->GetDevice()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline)));
    };
    graphics("VS", "PS", false, false, _capturePSO);
    graphics("SkyVS", "SkyPS", false, true, _skyPSO);
    graphics("ShadowVS", "ShadowPS", true, false, _shadowPSO);
    D3D_SHADER_MACRO culling[] = { { "PROBE_CULLING", "1" }, { nullptr, nullptr } };
    _cullPSO = GDX12RenderHelpers::CreateComputePipeline(_resources->Device, _cullRoot.get(),
        SHADERS_FOLDER "ReflectionCapture.hlsl", "CullCS", culling);
    _clearPSO = GDX12RenderHelpers::CreateComputePipeline(_resources->Device, _cullRoot.get(),
        SHADERS_FOLDER "ReflectionCapture.hlsl", "ClearCS", culling);
    D3D_SHADER_MACRO filtering[] = { { "LOCAL_PROBE_FILTER", "1" }, { nullptr, nullptr } };
    _filterPSO = GDX12RenderHelpers::CreateComputePipeline(_resources->Device, _filterRoot.get(),
        SHADERS_FOLDER "EnvironmentIBL.hlsl", "PrefilterCS", filtering);
    _signature = GDX12RenderHelpers::CreateIndexedDrawSignature(_resources->Device, _captureRoot.get());

    _frames.resize(_resources->FrameConstants.size());
    for (auto& frame : _frames)
    {
        frame.Cameras = std::make_unique<GDX12UploadBuffer<GDX12CameraConstants>>(_resources->Device, MaxReflectionProbes * 6);
        frame.Shadow = std::make_unique<GDX12UploadBuffer<GDX12ShadowConstants>>(_resources->Device, 1);
        frame.Counter = std::make_unique<GDX12UploadBuffer<UINT>>(_resources->Device, 1, EBufferType::Default, false);
        frame.Counter->CreateUAV(_resources->SRV_UAV_Heap.get(), _resources->SRV_UAV_Heap->GetAvailableIndex(ConstantsResources));
    }
}

void GDX12ReflectionProbePass::CreateTexture(GDX12Texture& texture, const UINT size, const UINT slices, const UINT mips,
    const D3D12_RESOURCE_FLAGS flags, const bool depth)
{
    const DXGI_FORMAT format = depth ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_R16G16B16A16_FLOAT;
    const auto resource = CD3DX12_RESOURCE_DESC::Tex2D(format, size, size,
        static_cast<UINT16>(slices), static_cast<UINT16>(mips), 1, 0, flags);
    D3D12_CLEAR_VALUE clear = {};
    clear.Format = depth ? DXGI_FORMAT_D32_FLOAT : format;
    GDX12TextureDesc desc;
    const bool clearable = (flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) != 0;
    desc.ExternalResource = _resources->Device->CreateResource(D3D12_HEAP_TYPE_DEFAULT, resource,
        D3D12_RESOURCE_STATE_COMMON, clearable ? &clear : nullptr);
    desc.Width = desc.Height = size;
    desc.Format = depth ? DXGI_FORMAT_R32_FLOAT : format;
    desc.CreateSRV = _capture && &texture == &_capture->SunShadow;
    if (desc.CreateSRV)
    {
        desc.SRV_UAV_Heap = _resources->SRV_UAV_Heap.get();
        desc.SRVHeapIndex = texture.IsInitialized() ? texture.GetSRV()->HeapIndex :
            desc.SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength);
        desc.SRVDesc.Format = desc.Format;
        desc.SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.SRVDesc.ViewDimension = depth ? D3D12_SRV_DIMENSION_TEXTURE2D : D3D12_SRV_DIMENSION_TEXTURECUBE;
        if (depth) desc.SRVDesc.Texture2D.MipLevels = mips;
        else desc.SRVDesc.TextureCube.MipLevels = mips;
    }
    const UINT64 previousBytes = AllocationBytes(_resources->Device, &texture);
    texture.Initialize(desc);
    D3D12_RESOURCE_ALLOCATION_INFO allocation = _resources->Device->GetDevice()->GetResourceAllocationInfo(0, 1, &resource);
    _statistics.TextureBytes = _statistics.TextureBytes - previousBytes + allocation.SizeInBytes;
}

void GDX12ReflectionProbePass::CreateFaceViews(GDX12Texture& texture,
    std::vector<std::unique_ptr<GDX12Descriptor>>& views, bool depth)
{
    views.clear();
    const auto resource = texture.GetResource()->D3DResource;
    const auto desc = resource->GetDesc();
    for (UINT face = 0; face < desc.DepthOrArraySize; ++face)
        for (UINT mip = 0; mip < desc.MipLevels; ++mip)
        {
            auto descriptor = std::make_unique<GDX12Descriptor>();
            if (depth)
            {
                D3D12_DEPTH_STENCIL_VIEW_DESC view = {};
                view.Format = DXGI_FORMAT_D32_FLOAT;
                view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
                view.Texture2DArray.FirstArraySlice = face;
                view.Texture2DArray.ArraySize = 1;
                view.Texture2DArray.MipSlice = mip;
                descriptor->InitAsDSV(resource.Get(), &view, _resources->DSVHeap.get(), _resources->DSVHeap->GetAvailableIndex());
            }
            else
            {
                D3D12_RENDER_TARGET_VIEW_DESC view = {};
                view.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                view.Texture2DArray.FirstArraySlice = face;
                view.Texture2DArray.ArraySize = 1;
                view.Texture2DArray.MipSlice = mip;
                descriptor->InitAsRTV(resource.Get(), &view, _resources->RTVHeap.get(), _resources->RTVHeap->GetAvailableIndex());
            }
            views.push_back(std::move(descriptor));
        }
}

void GDX12ReflectionProbePass::CreatePublishedTexture(UINT count, UINT resolution)
{
    _resources->Device->GetCommandQueue()->Flush();
    _publishedResolution = resolution;
    _publishedMips = GDX12RenderHelpers::MipCount(resolution);
    CreateTexture(_published, resolution, count * 6, _publishedMips, D3D12_RESOURCE_FLAG_NONE, false);
    _published.GetResource()->D3DResource->SetName(L"Reflection published radiance");
    _publishedNeedsTransition = true;
    D3D12_SHADER_RESOURCE_VIEW_DESC view = {};
    view.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
    view.TextureCubeArray.NumCubes = count;
    view.TextureCubeArray.MipLevels = _publishedMips;
    _resources->ReflectionRadianceSRV.InitAsSRV(_published.GetResource()->D3DResource.Get(), &view,
        _resources->SRV_UAV_Heap.get(), _resources->ReflectionRadianceSRV.HeapIndex);
    for (auto& probe : _probes)
    {
        probe.Ready = false;
        probe.LastUpdate = 0;
    }
    _activeProbe = _roundRobinProbe = 0;
    _scheduler.Reset();
}

void GDX12ReflectionProbePass::SelectProbe(UINT probe)
{
    _activeProbe = probe;
    _resolution = _probes[probe].Source.Resolution;
    _mips = GDX12RenderHelpers::MipCount(_resolution);
}

void GDX12ReflectionProbePass::CreateCaptureTextures(UINT resolution)
{
    for (auto& probe : _probes)
    {
        if (probe.OwnsCapture || !probe.Raw) continue;
        _resources->Device->GetCommandQueue()->Flush();
        _statistics.TextureBytes -= AllocationBytes(_resources->Device, probe.Raw.get()) +
            AllocationBytes(_resources->Device, probe.BakedColor.get()) + AllocationBytes(_resources->Device, probe.BakedDepth.get());
        probe.RawRTVs.clear();
        probe.RawMipSRVs.clear();
        probe.BakedRTVs.clear();
        probe.BakedDSVs.clear();
        probe.Raw.reset();
        probe.BakedColor.reset();
        probe.BakedDepth.reset();
        probe.CapturedFaces = probe.BakedFaces = probe.NextFace = 0;
        probe.RebakePending = false;
    }
    if (!resolution)
    {
        if (_capture)
        {
            _resources->Device->GetCommandQueue()->Flush();
            for (auto* texture : {&_capture->Filtered, &_capture->Depth, &_capture->SunShadow})
                _statistics.TextureBytes -= AllocationBytes(_resources->Device, texture);
            _filterUAVs.clear();
            _depthDSVs.clear();
            _sunDSV.clear();
            _capture.reset();
            _captureTextureResolution = 0;
        }
        return;
    }
    if (!_capture) _capture.emplace();
    auto& sun = _capture->SunShadow;

    const bool sunEnabled = (_resources->Lighting.SunShadowFlags & 1u) != 0;
    const UINT sunSize = sunEnabled ? (std::max)(1u, _resources->ShadowQuality.SunResolution / 2) : 1u;
    if (!sun.IsInitialized() || sun.GetWidth() != sunSize)
    {
        if (sun.IsInitialized()) _resources->Device->GetCommandQueue()->Flush();
        CreateTexture(sun, sunSize, 1, 1, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, true);
        CreateFaceViews(sun, _sunDSV, true);
        _sunShadowNeedsClear = true;
    }
    const UINT captureMips = GDX12RenderHelpers::MipCount(resolution);
    if (_captureTextureResolution != resolution)
    {
        _resources->Device->GetCommandQueue()->Flush();
        _captureTextureResolution = resolution;
        CreateTexture(_capture->Filtered, resolution, 6, captureMips, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, false);
        CreateTexture(_capture->Depth, resolution, 6, captureMips, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, true);
        CreateFaceViews(_capture->Depth, _depthDSVs, true);
        _filterUAVs.clear();
        for (UINT mip = 0; mip < captureMips; ++mip)
        {
            auto descriptor = std::make_unique<GDX12Descriptor>();
            D3D12_UNORDERED_ACCESS_VIEW_DESC view = {};
            view.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
            view.Texture2DArray.ArraySize = 6;
            view.Texture2DArray.MipSlice = mip;
            descriptor->InitAsUAV(_capture->Filtered.GetResource()->D3DResource.Get(), nullptr, &view,
                _resources->SRV_UAV_Heap.get(), _resources->SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength));
            _filterUAVs.push_back(std::move(descriptor));
        }
    }
    for (auto& probe : _probes)
    {
        if (!probe.OwnsCapture || (probe.Raw && probe.Raw->IsInitialized() && probe.Raw->GetWidth() == resolution)) continue;
        _resources->Device->GetCommandQueue()->Flush();
        if (!probe.Raw) probe.Raw = std::make_unique<GDX12Texture>();
        CreateTexture(*probe.Raw, resolution, 6, captureMips, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, false);
        CreateFaceViews(*probe.Raw, probe.RawRTVs, false);
        probe.CapturedFaces = probe.NextFace = 0;
        _scheduler.Reset();
        probe.RawMipSRVs.clear();
        for (UINT mip = 0; mip < captureMips; ++mip)
        {
            auto descriptor = std::make_unique<GDX12Descriptor>();
            D3D12_SHADER_RESOURCE_VIEW_DESC view = {};
            view.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
            view.TextureCube.MostDetailedMip = mip;
            view.TextureCube.MipLevels = 1;
            descriptor->InitAsSRV(probe.Raw->GetResource()->D3DResource.Get(), &view,
                _resources->SRV_UAV_Heap.get(), _resources->SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength));
            probe.RawMipSRVs.push_back(std::move(descriptor));
        }
    }
}

void GDX12ReflectionProbePass::CreateBakedTextures(ProbeState& probe)
{
    const UINT resolution = probe.Source.Resolution;
    if (probe.BakedColor && probe.BakedColor->GetWidth() == resolution) return;
    _resources->Device->GetCommandQueue()->Flush();
    if (!probe.BakedColor)
    {
        probe.BakedColor = std::make_unique<GDX12Texture>();
        probe.BakedDepth = std::make_unique<GDX12Texture>();
    }
    CreateTexture(*probe.BakedColor, resolution, 6, 1, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, false);
    CreateTexture(*probe.BakedDepth, resolution, 6, 1, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, true);
    probe.BakedColor->GetResource()->D3DResource->SetName(L"Reflection probe baked color");
    probe.BakedDepth->GetResource()->D3DResource->SetName(L"Reflection probe baked depth");
    CreateFaceViews(*probe.BakedColor, probe.BakedRTVs, false);
    CreateFaceViews(*probe.BakedDepth, probe.BakedDSVs, true);
    probe.BakedFaces = 0;
}

std::pair<size_t, size_t> GDX12ReflectionProbePass::GetReadiness()
{
    if (!_enabled) return {};
    const auto ready = std::count_if(_probes.begin(), _probes.end(),
        [](const ProbeState& probe) { return probe.Ready && probe.LastUpdate != 0; });
    return {static_cast<size_t>(ready), _probes.size()};
}

void GDX12ReflectionProbePass::Prepare()
{
    GDX12_SCOPED_CPU_EVENT("Reflection probe preparation");
    if (_planPrepared) return;
    _views.clear();
    _capturePlanned = false;
    ++_frameNumber;
    _statistics.CapturedFaces = _statistics.BakedFaces = _statistics.PublishedProbes = 0;
    const auto settings = _externalSettings ? *_externalSettings : GetReflectionSettings();
    if (!settings.Enabled || _resources->ReflectionProbes.empty())
    {
        if (_enabled)
        {
            for (auto& probe : _probes)
            {
                probe.Ready = probe.OwnsCapture = false;
                probe.LastUpdate = 0;
            }
            CreateCaptureTextures(0);
        }
        if (_enabled) { _commonData->ResetHistory = true; ++_commonData->HistoryRevision; }
        _enabled = false;
        _scheduler.Reset();
        _planPrepared = true;
        return;
    }
    auto submitted = _resources->ReflectionProbes;
    if (submitted.size() > MaxReflectionProbes) throw std::runtime_error("Too many reflection probes (maximum 32)");
    UINT maximumResolution = 0;
    for (auto& probe : submitted)
    {
        if (!IsValidReflectionProbe(probe)) throw std::invalid_argument("Invalid reflection probe bounds or capture parameters");
        static_cast<GDX12ReflectionCaptureSettings&>(probe) = ResolveReflectionCaptureSettings(probe, settings.Quality);
        maximumResolution = (std::max)(maximumResolution, probe.Resolution);
    }
    bool topologyChanged = _probes.size() != submitted.size();
    if (!topologyChanged)
        for (size_t i = 0; i < submitted.size(); ++i) topologyChanged |= _probes[i].Source.Id != submitted[i].Id;
    if (!_enabled || topologyChanged || _settings.Quality != settings.Quality)
    {
        _commonData->ResetHistory = true;
        ++_commonData->HistoryRevision;
    }
    if (topologyChanged)
    {
        _resources->Device->GetCommandQueue()->Flush();
        for (auto& probe : _probes)
            _statistics.TextureBytes -= AllocationBytes(_resources->Device, probe.BakedColor.get()) +
                AllocationBytes(_resources->Device, probe.BakedDepth.get()) + AllocationBytes(_resources->Device, probe.Raw.get());
        _probes.clear();
        _probes.resize(submitted.size());
        _activeProbe = _roundRobinProbe = 0;
        _scheduler.Reset();
    }
    if (topologyChanged || _publishedResolution != maximumResolution)
        CreatePublishedTexture(static_cast<UINT>(submitted.size()), maximumResolution);
    const bool invalidate = _staticRevision != _resources->ReflectionStaticRevision ||
        _environmentRevision != _resources->ReflectionEnvironmentRevision;
    const bool staticGeometryChanged = _staticGeometryRevision != _resources->ReflectionStaticGeometryRevision;
    const bool shadowQualityChanged = !(_shadowQuality == _resources->ShadowQuality);
    UINT maximumCaptureResolution = 0;
    for (UINT i = 0; i < submitted.size(); ++i)
    {
        auto& probe = _probes[i];
        if (!SameReflectionCapture(probe.Source, submitted[i]) || shadowQualityChanged)
        {
            if (probe.Source.Adapter != submitted[i].Adapter) probe.Ready = false;
            _commonData->ResetHistory = true;
            ++_commonData->HistoryRevision;
            probe.CapturedFaces = probe.BakedFaces = probe.NextFace = 0;
            probe.RebakePending = false;
            probe.LastUpdate = 0;
        }
        else if (staticGeometryChanged && probe.Source.Implementation == 2)
        {
            probe.CapturedFaces = probe.BakedFaces = probe.NextFace = 0;
            probe.RebakePending = false;
            probe.LastUpdate = 0;
        }
        else if (staticGeometryChanged || (invalidate && probe.Source.Implementation != 2))
        {
            // Finish the current generation before rebaking so changing lighting cannot starve publication.
            if (probe.CapturedFaces) probe.RebakePending = true;
            else { probe.BakedFaces = 0; probe.LastUpdate = 0; }
        }
        probe.Source = submitted[i];
        probe.OwnsCapture = !_ownsCapture || _ownsCapture(i);
        if (probe.OwnsCapture) maximumCaptureResolution = (std::max)(maximumCaptureResolution, probe.Source.Resolution);
    }
    _staticRevision = _resources->ReflectionStaticRevision;
    _staticGeometryRevision = _resources->ReflectionStaticGeometryRevision;
    _environmentRevision = _resources->ReflectionEnvironmentRevision;
    _shadowQuality = _resources->ShadowQuality;
    if (!_enabled || _settings.Scheduled != settings.Scheduled) _scheduler.Reset();
    _settings = settings;
    _enabled = true;
    CreateCaptureTextures(maximumCaptureResolution);
    std::vector<GDX12ReflectionSchedule::Work> work(_probes.size());
    if (settings.Scheduled)
    {
        const auto camera = _commonData->CameraView.Invert().Translation();
        std::vector<GDX12ReflectionSchedule::Probe> schedule;
        for (const auto& state : _probes)
        {
            if (!state.OwnsCapture) continue;
            const auto& probe = state.Source;
            schedule.push_back({probe.Id, GDX12ReflectionSchedule::DistanceToBox({camera.x,camera.y,camera.z},
                {probe.BoxMin.x,probe.BoxMin.y,probe.BoxMin.z}, {probe.BoxMax.x,probe.BoxMax.y,probe.BoxMax.z})});
        }
        const auto scheduled = _scheduler.Select(schedule, settings.Policy);
        for (size_t i = 0, owned = 0; i < _probes.size(); ++i)
            if (_probes[i].OwnsCapture) work[i] = scheduled[owned++];
    }
    else
    {
        for (size_t visited = 0; visited < _probes.size(); ++visited)
        {
            auto& probe = _probes[_roundRobinProbe];
            if (probe.OwnsCapture && (!_captureAllowed || _captureAllowed(_roundRobinProbe)) &&
                (!probe.Ready || probe.CapturedFaces || !probe.LastUpdate ||
                 (probe.Source.Implementation != 2 && _frameNumber - probe.LastUpdate >= probe.Source.UpdateInterval)))
            {
                const auto end = (std::min)(6u, probe.NextFace + probe.Source.FacesPerFrame);
                for (UINT face = probe.NextFace; face < end; ++face) work[_roundRobinProbe].Faces |= 1u << face;
                probe.NextFace = end % 6;
                break;
            }
            _roundRobinProbe = (_roundRobinProbe + 1) % static_cast<UINT>(_probes.size());
        }
    }
    // Scheduled priority may select an already frozen probe; only an explicit invalidation reopens it.
    for (UINT index = 0; index < _probes.size(); ++index)
        if (!_probes[index].OwnsCapture || (_captureAllowed && !_captureAllowed(index)) ||
            (_probes[index].Source.Implementation == 2 && _probes[index].Ready &&
             _probes[index].LastUpdate && !_probes[index].CapturedFaces))
            work[index].Faces = 0;
    if (std::none_of(work.begin(), work.end(), [](const auto& item) { return item.Faces != 0; }))
    {
        _planPrepared = true;
        return;
    }
    _views.resize(_probes.size() * 14);
    auto& frame = _frames[_resources->CurrFrameConstantsIndex];
    const bool sunShadow = (_resources->Lighting.SunShadowFlags & 1u) != 0;
    const auto shadowSettings = sunShadow ? ProbeSunShadow(*_resources, _capture->SunShadow.GetWidth()) : GDX12ShadowConstants{};
    frame.Shadow->CopyData(0, shadowSettings);
    for (UINT index = 0; index < _probes.size(); ++index)
    {
        auto& state = _probes[index];
        if (!work[index].Faces) continue;
        assert(state.OwnsCapture && state.Raw);
        SelectProbe(index);
        for (UINT face = 0; face < 6; ++face)
        {
            frame.Cameras->CopyData(index * 6 + face, FaceCamera(state.Source, face, _resolution));
        }
        _capturePlanned = true;
        const bool bake = state.Source.Implementation == 1 && state.BakedFaces != 63;
        const bool frozen = state.Source.Implementation == 2;
        if (bake) CreateBakedTextures(state);
        const auto select = [&](UINT localSlot, Geometry geometry, bool shadow, UINT face, bool baked)
        {
            auto& prepared = _views[index * 14 + localSlot];
            prepared.Probe = index;
            prepared.Filter = geometry;
            prepared.Face = face;
            prepared.Bake = baked;
            prepared.Selected = true;
            auto& view = prepared.Context;
            view.Stage = shadow ? GDX12GeometryStage::Shadow : GDX12GeometryStage::ReflectionCapture;
            view.ViewProjection = shadow ? Matrix(shadowSettings.ViewProj[0]).Transpose() :
                Matrix(FaceCamera(state.Source, face, _resolution).ViewProj).Transpose();
            view.Eye = state.Source.Position;
            view.CameraConstants = shadow ? 0 : frame.Cameras->GetElementAddress(index * 6 + face);
            view.Filter = geometry == StaticGeometry ? GDX12GeometryFilter::Static :
                (geometry == DynamicGeometry ? GDX12GeometryFilter::Dynamic : GDX12GeometryFilter::All);
            view.Key = shadow ? (4ull << 60) | (index * 3ull + geometry) :
                (5ull << 60) | (static_cast<uint64_t>(index) << 8) | (geometry * 6u + face);
            view.FrameSlot = _resources->CurrFrameConstantsIndex;
            view.DepthFormat = DXGI_FORMAT_D32_FLOAT;
            if (!shadow)
            {
                prepared.Lighting.SunShadow = &_capture->SunShadow;
                prepared.Lighting.Irradiance = _inputs[0]->GetTexture();
                prepared.Lighting.Prefiltered = _inputs[1]->GetTexture();
                prepared.Lighting.BRDF = _inputs[2]->GetTexture();
                prepared.Lighting.ShadowConstants = frame.Shadow->GetElementAddress(0);
                view.CaptureLighting = &prepared.Lighting;
                view.ColorTargetCount = 1;
                view.ColorFormats[0] = baked ? state.BakedColor->GetFormat() : state.Raw->GetFormat();
            }
        };
        if (bake)
        {
            if (sunShadow) select(0, StaticGeometry, true, 0, false);
            for (UINT face = 0; face < 6; ++face)
                if ((work[index].Faces & (1u << face)) && !(state.BakedFaces & (1u << face)))
                    select(1 + face, StaticGeometry, false, face, true);
        }
        if (sunShadow) select(7, frozen ? StaticGeometry : AllGeometry, true, 0, false);
        for (UINT face = 0; face < 6; ++face)
            if (work[index].Faces & (1u << face))
                select(8 + face, frozen ? StaticGeometry : state.Source.Implementation == 1 ? DynamicGeometry : AllGeometry,
                    false, face, false);
    }
    _planPrepared = true;
}

void GDX12ReflectionProbePass::ResetHistory()
{
    for (auto& probe : _probes)
    {
        probe.Ready = false;
        probe.CapturedFaces = probe.BakedFaces = probe.NextFace = 0;
        probe.LastUpdate = 0;
        probe.RebakePending = false;
    }
    _scheduler.Reset();
    _roundRobinProbe = 0;
    _frameNumber = 0;
    _planPrepared = _capturePlanned = false;
}

void GDX12ReflectionProbePass::ClearDenendencies()
{
    GDX12RenderPass::ClearDenendencies();
    _externalSettings = nullptr;
    _ownsCapture = {};
    _captureAllowed = {};
    _frames.clear();
    _probes.clear();
    _views.clear();
    _scheduler.Reset();
    _planPrepared = _capturePlanned = false;
}

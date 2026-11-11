#include "Engine.RendererDX12/RenderPasses/GDX12ReflectionProbePass.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <stdexcept>

namespace
{
    UINT CubeMipCount(UINT resolution)
    {
        UINT count = 1;
        for (; resolution > 1; resolution >>= 1) ++count;
        return count;
    }

    UINT64 AllocationBytes(GDX12Device* device, GDX12Texture* texture)
    {
        if (!texture || !texture->IsInitialized()) return 0;
        const auto desc = texture->GetResource()->D3DResource->GetDesc();
        return device->GetDevice()->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
    }

    bool SameCapture(const GDX12ReflectionProbe& a, const GDX12ReflectionProbe& b)
    {
        return a.Id == b.Id && a.Position == b.Position && a.BoxMin == b.BoxMin && a.BoxMax == b.BoxMax &&
            a.NearPlane == b.NearPlane && a.FarPlane == b.FarPlane && a.Revision == b.Revision &&
            a.Implementation == b.Implementation && a.Resolution == b.Resolution &&
            a.PrefilterSamples == b.PrefilterSamples;
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
        XMStoreFloat4x4(&result.ViewProj[0], (view * projection).Transpose());
        result.TexelWorldSize.x = (std::max)(maximum.x - minimum.x, maximum.y - minimum.y) / resolution;
        return result;
    }
}

GDX12ReflectionProbePass::GDX12ReflectionProbePass()
{
    _flags = RENDER_PASS_FLAG_USE_GEOMETRY | RENDER_PASS_FLAG_USE_MATERIALS | RENDER_PASS_FLAG_USE_INSTANCES;
    _numInputs = 5;
    _numOutputs = 1;
    _outputs.push_back(&_published);
}

void GDX12ReflectionProbePass::Initialize()
{
    GDX12RootSignatureDesc capture;
    capture.InstancedGeometry = true;
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
        frame.Cameras = std::make_unique<GDX12UploadBuffer<GDX12CameraConstants>>(_resources->Device, 6);
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
    desc.CreateSRV = &texture == &_sunShadow;
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
    _publishedMips = CubeMipCount(resolution);
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
    _activeProbe = _face = 0;
}

void GDX12ReflectionProbePass::CreateCaptureTextures(UINT resolution)
{
    const UINT sunSize = (std::max)(1u, _resources->ShadowQuality.SunResolution / 2);
    if (!_sunShadow.IsInitialized() || _sunShadow.GetWidth() != sunSize)
    {
        CreateTexture(_sunShadow, sunSize, 1, 1, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, true);
        _sunShadow.GetResource()->D3DResource->SetName(L"Reflection sun shadow");
        CreateFaceViews(_sunShadow, _sunDSV, true);
        _sunShadowNeedsClear = true;
    }
    if (_captureTextureResolution == resolution) return;
    _resources->Device->GetCommandQueue()->Flush();
    _captureTextureResolution = resolution;
    const UINT captureMips = CubeMipCount(resolution);
    CreateTexture(_raw, resolution, 6, captureMips, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, false);
    CreateTexture(_filtered, resolution, 6, captureMips, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, false);
    CreateTexture(_depth, resolution, 6, captureMips, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, true);
    _raw.GetResource()->D3DResource->SetName(L"Reflection working capture");
    _filtered.GetResource()->D3DResource->SetName(L"Reflection working GGX");
    _depth.GetResource()->D3DResource->SetName(L"Reflection restored depth");
    CreateFaceViews(_raw, _rawRTVs, false);
    CreateFaceViews(_depth, _depthDSVs, true);
    _filterUAVs.clear();
    _rawMipSRVs.clear();
    for (UINT mip = 0; mip < captureMips; ++mip)
    {
        auto sourceView = std::make_unique<GDX12Descriptor>();
        D3D12_SHADER_RESOURCE_VIEW_DESC sourceDesc = {};
        sourceDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        sourceDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sourceDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        sourceDesc.TextureCube.MostDetailedMip = mip;
        sourceDesc.TextureCube.MipLevels = 1;
        sourceView->InitAsSRV(_raw.GetResource()->D3DResource.Get(), &sourceDesc,
            _resources->SRV_UAV_Heap.get(), _resources->SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength));
        _rawMipSRVs.push_back(std::move(sourceView));
        auto descriptor = std::make_unique<GDX12Descriptor>();
        D3D12_UNORDERED_ACCESS_VIEW_DESC view = {};
        view.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
        view.Texture2DArray.ArraySize = 6;
        view.Texture2DArray.MipSlice = mip;
        descriptor->InitAsUAV(_filtered.GetResource()->D3DResource.Get(), nullptr, &view,
            _resources->SRV_UAV_Heap.get(), _resources->SRV_UAV_Heap->GetAvailableIndex(TextureResources_StartIndex, TextureResources_RangeLength));
        _filterUAVs.push_back(std::move(descriptor));
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

void GDX12ReflectionProbePass::Cull(GDX12CommandList* commands, const UINT camera, const Geometry geometry, const bool shadow)
{
    auto& frame = _frames[_resources->CurrFrameConstantsIndex];
    auto& constants = _resources->FrameConstants[_resources->CurrFrameConstantsIndex];
    const UINT count = _resources->IndirectCommandsCache->GetElementCount();
    if (!frame.Commands || frame.Commands->GetElementCount() < count)
    {
        frame.Commands = std::make_unique<GDX12UploadBuffer<GDX12IndirectDrawArgs>>(_resources->Device,
            (std::max)(count, 1u), EBufferType::Default, false);
        frame.Commands->CreateUAV(_resources->SRV_UAV_Heap.get(), _resources->SRV_UAV_Heap->GetAvailableIndex(ConstantsResources));
    }
    commands->ResourceBarrier({ frame.Commands->GetResource().GetUnorderedAccessBarrier(), frame.Counter->GetResource().GetUnorderedAccessBarrier() });
    commands->SetComputeRootSignature(_cullRoot.get());
    commands->SetComputeUAV(0, frame.Commands->GetUAV()->GPUHandle);
    commands->SetComputeUAV(1, frame.Counter->GetUAV()->GPUHandle);
    commands->SetPipelineState(_clearPSO);
    commands->Dispatch(1, 1, 1);
    commands->ResourceBarrier({ frame.Counter->GetResource().GetUAVBarrier() });
    commands->SetPipelineState(_cullPSO);
    const UINT parameters[] = { static_cast<UINT>(geometry), shadow ? 1u : 0u };
    commands->GetCommandList()->SetComputeRoot32BitConstants(0, 2, parameters, 0);
    commands->SetComputeRootConstantBufferView(1, frame.Cameras->GetElementAddress(camera));
    commands->SetComputeRootConstantBufferView(2, frame.Shadow->GetElementAddress(0));
    commands->SetComputeSRV(0, constants->ShadowInstances->GetSRV()->GPUHandle);
    commands->SetComputeSRV(1, constants->ShadowTemplates->GetSRV()->GPUHandle);
    commands->SetComputeSRV(2, constants->ShadowMaterials->GetSRV()->GPUHandle);
    if (count) commands->Dispatch((count + 63) / 64, 1, 1);
    if (_resources->SkeletalBatchCount)
    {
        if (!frame.Batches) frame.Batches = std::make_unique<GDX12InstanceBatcher>(_resources->Device);
        frame.Batches->Build(commands, *constants->ShadowInstances, *frame.Commands, *frame.Counter,
            _resources->SkeletalBatchCount, _resources->SkeletalInstanceCount);
    }
    else frame.Batches.reset();
    commands->ResourceBarrier({ frame.Commands->GetResource().GetIndirectArgsBarrier(), frame.Counter->GetResource().GetIndirectArgsBarrier() });
}

GDX12GraphicsBindings GDX12ReflectionProbePass::CaptureBindings(const UINT face, GDX12Texture* localShadows)
{
    auto& frame = _frames[_resources->CurrFrameConstantsIndex];
    auto& constants = _resources->FrameConstants[_resources->CurrFrameConstantsIndex];
    GDX12GraphicsBindings binding;
    binding.Root = _captureRoot.get();
    const auto geometryAddress = _resources->GeometryBuffer->GetVertexAddress();
    const auto geometrySlot = static_cast<UINT>(binding.Root->GetGeometryRootParamIndex());
    binding.RootSRVs = { { geometrySlot, geometryAddress }, { geometrySlot + 1,
        frame.Batches ? frame.Batches->VisibleInstancesAddress() : geometryAddress } };
    binding.Pipeline = _capturePSO;
    binding.IndirectSignature = _signature.Get();
    binding.IndirectStride = sizeof(GDX12IndirectDrawArgs);
    binding.Stage = GDX12GeometryStage::ReflectionCapture;
    binding.ColorTargetCount = 1;
    binding.ColorFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    binding.DepthFormat = DXGI_FORMAT_D32_FLOAT;
    binding.Heap = _resources->SRV_UAV_Heap.get();
    binding.CBVs.push_back({ 1, constants->MainCB->GetElementAddress(0) });
    binding.CBVs.push_back({ 2, frame.Cameras->GetElementAddress(face) });
    binding.CBVs.push_back({ 3, frame.Shadow->GetElementAddress(0) });
    binding.SRVs.push_back({ 0, constants->ShadowMaterials->GetSRV()->GPUHandle });
    binding.SRVs.push_back({ 1, constants->ShadowTransforms->GetSRV()->GPUHandle });
    binding.SRVs.push_back({ 2, constants->ShadowInstances->GetSRV()->GPUHandle });
    binding.SRVs.push_back({ 3, _sunShadow.GetSRV()->GPUHandle });
    for (UINT i = 0; i < 3; ++i) binding.SRVs.push_back({ 4 + i, _inputs[i]->GetTexture()->GetSRV()->GPUHandle });
    binding.SRVs.push_back({ 7, constants->ShadowLights->GetSRV()->GPUHandle });
    binding.SRVs.push_back({ 8, localShadows->GetSRV()->GPUHandle });
    binding.SRVs.push_back({ 9, _inputs[3]->GetTexture()->GetSRV()->GPUHandle });
    binding.CBVs.push_back({ 4, _resources->FrameConstants[_resources->CurrFrameConstantsIndex]->SkyCB->GetElementAddress(0) });
    binding.SRVs.push_back({ 10, _resources->SkyTransmittanceSRV.GPUHandle });
    binding.SRVs.push_back({ 11, _resources->SRV_UAV_Heap->GetGPUHandle(Texture2D_StartIndex) });
    return binding;
}

const GDX12GeometryContext* GDX12ReflectionProbePass::GetGeometryView(const UINT slot) const
{
    return _planPrepared && slot < _views.size() && _views[slot].Selected ? &_views[slot].Context : nullptr;
}

void GDX12ReflectionProbePass::BindGeometryViewTargets(GDX12CommandList* commands, const UINT slot)
{
    const auto* view = GetGeometryView(slot);
    if (!view) return;
    commands->SetDescriptorHeaps({ _resources->SRV_UAV_Heap.get() });
    if (view->Stage == GDX12GeometryStage::Shadow)
    {
        commands->ResourceBarrier({ _sunShadow.GetResource()->GetDepthWriteBarrier() });
        const auto dsv = _sunDSV[0]->CPUHandle;
        commands->GetCommandList()->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
        commands->SetViewport(_sunShadow.GetViewport());
        commands->SetScissorRect(_sunShadow.GetScissorRect());
        return;
    }
    const auto& prepared = _views[slot];
    auto& state = _probes[_activeProbe];
    const UINT captureSlice = prepared.Face * _publishedMips + (_publishedMips - _mips);
    auto& color = prepared.Bake ? *state.BakedColor : _raw;
    auto& depth = prepared.Bake ? *state.BakedDepth : _depth;
    const auto rtv = (prepared.Bake ? state.BakedRTVs[prepared.Face] : _rawRTVs[captureSlice])->CPUHandle;
    const auto dsv = (prepared.Bake ? state.BakedDSVs[prepared.Face] : _depthDSVs[captureSlice])->CPUHandle;
    commands->ResourceBarrier({ _sunShadow.GetResource()->GetSRVBarrier(),
        color.GetResource()->GetRenderTargetBarrier(), depth.GetResource()->GetDepthWriteBarrier() });
    commands->GetCommandList()->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    commands->SetViewport({ 0, 0, static_cast<float>(_resolution), static_cast<float>(_resolution), 0, 1 });
    commands->SetScissorRect({ 0, 0, static_cast<LONG>(_resolution), static_cast<LONG>(_resolution) });
}

void GDX12ReflectionProbePass::RenderShadow(GDX12CommandList* commands, const UINT slot)
{
    const auto& prepared = _views[slot];
    Cull(commands, 0, prepared.Filter, true);
    auto& constants = _resources->FrameConstants[_resources->CurrFrameConstantsIndex];
    auto& frame = _frames[_resources->CurrFrameConstantsIndex];
    GDX12GraphicsBindings binding;
    binding.Root = _captureRoot.get();
    const auto geometryAddress = _resources->GeometryBuffer->GetVertexAddress();
    const auto geometrySlot = static_cast<UINT>(binding.Root->GetGeometryRootParamIndex());
    binding.RootSRVs = { { geometrySlot, geometryAddress }, { geometrySlot + 1,
        frame.Batches ? frame.Batches->VisibleInstancesAddress() : geometryAddress } };
    binding.IndirectSignature = _signature.Get();
    binding.IndirectStride = sizeof(GDX12IndirectDrawArgs);
    binding.Stage = GDX12GeometryStage::Shadow;
    binding.DepthFormat = DXGI_FORMAT_D32_FLOAT;
    binding.Heap = _resources->SRV_UAV_Heap.get();
    binding.CBVs.push_back({ 3, frame.Shadow->GetElementAddress(0) });
    binding.SRVs.push_back({ 0, constants->ShadowMaterials->GetSRV()->GPUHandle });
    binding.SRVs.push_back({ 1, constants->ShadowTransforms->GetSRV()->GPUHandle });
    binding.SRVs.push_back({ 2, constants->ShadowInstances->GetSRV()->GPUHandle });
    binding.CBVs.push_back({ 4, constants->SkyCB->GetElementAddress(0) });
    binding.SRVs.push_back({ 10, _resources->SkyTransmittanceSRV.GPUHandle });
    binding.SRVs.push_back({ 11, _resources->SRV_UAV_Heap->GetGPUHandle(Texture2D_StartIndex) });
    binding.Pipeline = _shadowPSO;
    BindGeometryViewTargets(commands, slot);
    commands->GetCommandList()->ClearDepthStencilView(_sunDSV[0]->CPUHandle, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
    _sunShadowNeedsClear = false;
    const UINT count = _resources->IndirectCommandsCache->GetElementCount();
    auto view = prepared.Context;
    view.IncludeApplicationGeometry = !_splitGeometry;
    GDX12RenderBatch mesh;
    mesh.Binding = &binding;
    mesh.Geometry = _resources->GeometryBuffer.get();
    mesh.InstanceCount = count;
    mesh.Draw = { GDX12DrawKind::Indirect, {}, {}, _signature.Get(),
        count ? (frame.Batches ? frame.Batches->Commands() : *frame.Commands).GetResource().D3DResource.Get() : nullptr, 0,
        count ? frame.Counter->GetResource().D3DResource.Get() : nullptr, 0, count, sizeof(GDX12IndirectDrawArgs) };
    _resources->EmitGeometry(commands, view, mesh);
}

void GDX12ReflectionProbePass::RenderFace(GDX12CommandList* commands, const UINT slot)
{
    const auto& prepared = _views[slot];
    const UINT face = prepared.Face;
    const auto geometry = prepared.Filter;
    const bool bake = prepared.Bake;
    auto& state = _probes[_activeProbe];
    const UINT captureSlice = face * _publishedMips + (_publishedMips - _mips);
    auto& color = bake ? *state.BakedColor : _raw;
    auto& depth = bake ? *state.BakedDepth : _depth;
    const auto rtv = (bake ? state.BakedRTVs[face] : _rawRTVs[captureSlice])->CPUHandle;
    const auto dsv = (bake ? state.BakedDSVs[face] : _depthDSVs[captureSlice])->CPUHandle;
    if (!bake && geometry == DynamicGeometry)
    {
        commands->ResourceBarrier({ state.BakedColor->GetResource()->GetCopySourceBarrier(),
            state.BakedDepth->GetResource()->GetCopySourceBarrier(), color.GetResource()->GetCopyDestBarrier(), depth.GetResource()->GetCopyDestBarrier() });
        const CD3DX12_TEXTURE_COPY_LOCATION colorSource(state.BakedColor->GetResource()->D3DResource.Get(), face);
        const CD3DX12_TEXTURE_COPY_LOCATION depthSource(state.BakedDepth->GetResource()->D3DResource.Get(), face);
        const CD3DX12_TEXTURE_COPY_LOCATION colorDestination(color.GetResource()->D3DResource.Get(), captureSlice);
        const CD3DX12_TEXTURE_COPY_LOCATION depthDestination(depth.GetResource()->D3DResource.Get(), captureSlice);
        commands->GetCommandList()->CopyTextureRegion(&colorDestination, 0, 0, 0, &colorSource, nullptr);
        commands->GetCommandList()->CopyTextureRegion(&depthDestination, 0, 0, 0, &depthSource, nullptr);
    }
    Cull(commands, face, geometry, false);
    auto binding = CaptureBindings(face, prepared.Lighting.LocalShadows);
    BindGeometryViewTargets(commands, slot);
    binding.Bind(commands);
    commands->SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    if (geometry != DynamicGeometry)
    {
        const float clear[] = { 0, 0, 0, 0 };
        commands->GetCommandList()->ClearRenderTargetView(rtv, clear, 0, nullptr);
        commands->GetCommandList()->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0, 0, 0, nullptr);
        commands->SetPipelineState(_skyPSO);
        commands->DrawInstanced(3, 1, 0, 0);
    }
    auto& frame = _frames[_resources->CurrFrameConstantsIndex];
    const UINT count = _resources->IndirectCommandsCache->GetElementCount();
    auto view = prepared.Context;
    view.IncludeApplicationGeometry = !_splitGeometry;
    GDX12RenderBatch mesh;
    mesh.Binding = &binding;
    mesh.Geometry = _resources->GeometryBuffer.get();
    mesh.InstanceCount = count;
    mesh.Draw = { GDX12DrawKind::Indirect, {}, {}, _signature.Get(),
        count ? (frame.Batches ? frame.Batches->Commands() : *frame.Commands).GetResource().D3DResource.Get() : nullptr, 0,
        count ? frame.Counter->GetResource().D3DResource.Get() : nullptr, 0, count, sizeof(GDX12IndirectDrawArgs) };
    _resources->EmitGeometry(commands, view, mesh);
}

void GDX12ReflectionProbePass::ExecuteGeometryView(GDX12CommandList* commands, const UINT slot)
{
    const auto* view = GetGeometryView(slot);
    if (!view) return;
    if (view->Stage == GDX12GeometryStage::Shadow) RenderShadow(commands, slot);
    else RenderFace(commands, slot);
}

void GDX12ReflectionProbePass::FilterAndPublish(GDX12CommandList* commands, const UINT probe)
{
    commands->ResourceBarrier({ _raw.GetResource()->GetSRVBarrier(), _filtered.GetResource()->GetUnorderedAccessBarrier() });
    commands->SetComputeRootSignature(_filterRoot.get());
    commands->SetPipelineState(_filterPSO);
    // shortcut: capture input has one mip, add input mips if small HDR emitters alias at the chosen sample budget.
    commands->SetComputeSRV(0, _rawMipSRVs[_publishedMips - _mips]->GPUHandle);
    for (UINT mip = 0; mip < _mips; ++mip)
    {
        struct { UINT Size; float Roughness; UINT Samples; } parameters = {
            _resolution >> mip, static_cast<float>(mip) / (_mips - 1), _probes[probe].Source.PrefilterSamples };
        commands->GetCommandList()->SetComputeRoot32BitConstants(0, 3, &parameters, 0);
        commands->SetComputeUAV(0, _filterUAVs[_publishedMips - _mips + mip]->GPUHandle);
        commands->Dispatch((parameters.Size + 7) / 8, (parameters.Size + 7) / 8, 6);
    }
    commands->ResourceBarrier({ _filtered.GetResource()->GetCopySourceBarrier(), _published.GetResource()->GetCopyDestBarrier() });
    for (UINT face = 0; face < 6; ++face)
        for (UINT mip = 0; mip < _mips; ++mip)
        {
            const CD3DX12_TEXTURE_COPY_LOCATION source(_filtered.GetResource()->D3DResource.Get(), face * _publishedMips + (_publishedMips - _mips) + mip);
            const CD3DX12_TEXTURE_COPY_LOCATION destination(_published.GetResource()->D3DResource.Get(), (probe * 6 + face) * _publishedMips + (_publishedMips - _mips) + mip);
            commands->GetCommandList()->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        }
    commands->ResourceBarrier({ _published.GetResource()->GetSRVBarrier() });
    auto& state = _probes[probe];
    state.Ready = true;
    state.Published = state.Source;
    state.LastUpdate = _frameNumber;
    ++_statistics.PublishedProbes;
}

void GDX12ReflectionProbePass::WriteProbeData(GDX12CommandList* commands, const bool enabled)
{
    auto& frame = _resources->FrameConstants[_resources->CurrFrameConstantsIndex];
    for (UINT i = 0; i < MaxReflectionProbes; ++i)
    {
        GDX12ReflectionProbeData data;
        if (enabled && i < _probes.size() && _probes[i].Ready)
        {
            const auto& probe = _probes[i].Published;
            data.Position = probe.Position;
            data.BoxMin = probe.BoxMin;
            data.BoxMax = probe.BoxMax;
            data.BlendDistance = _probes[i].Source.BlendDistance;
            data.Intensity = _probes[i].Source.Intensity;
            data.MaxMip = static_cast<float>(_publishedMips - 1);
            data.MinMip = static_cast<float>(_publishedMips - CubeMipCount(probe.Resolution));
        }
        frame->ReflectionProbesUpload->CopyData(i, data);
    }
    frame->UploadReflectionProbeData(commands);
}

std::pair<size_t, size_t> GDX12ReflectionProbePass::GetReadiness() const
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
    _views = {};
    _capturePlanned = _staticShadowsPlanned = false;
    ++_frameNumber;
    _statistics.CapturedFaces = _statistics.BakedFaces = _statistics.PublishedProbes = 0;
    const auto settings = GetReflectionSettings();
    if (!settings.Enabled || _resources->ReflectionProbes.empty())
    {
        if (_resources->ReflectionProbes.empty())
            for (auto& probe : _probes)
            {
                probe.Ready = false;
                probe.LastUpdate = 0;
            }
        if (_enabled)
        {
            _commonData->ResetHistory = true;
            ++_commonData->HistoryRevision;
        }
        _enabled = false;
        _face = 0;
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
                AllocationBytes(_resources->Device, probe.BakedDepth.get());
        _probes.clear();
        _probes.resize(submitted.size());
        _activeProbe = _face = 0;
    }
    if (topologyChanged || _publishedResolution != maximumResolution)
        CreatePublishedTexture(static_cast<UINT>(submitted.size()), maximumResolution);
    const bool invalidate = _staticRevision != _resources->ReflectionStaticRevision ||
        _environmentRevision != _resources->ReflectionEnvironmentRevision;
    const bool shadowQualityChanged = !(_shadowQuality == _resources->ShadowQuality);
    if (invalidate || shadowQualityChanged)
    {
        if (_staticLocalShadows) _staticLocalShadows->InvalidateCache();
        _face = 0;
        for (auto& probe : _probes)
        {
            if (shadowQualityChanged) probe.Ready = false;
            else if (probe.Source.Implementation == 2 && probe.Ready && probe.LastUpdate) continue;
            probe.BakedFaces = 0;
            probe.LastUpdate = 0;
        }
    }
    _staticRevision = _resources->ReflectionStaticRevision;
    _environmentRevision = _resources->ReflectionEnvironmentRevision;
    _shadowQuality = _resources->ShadowQuality;
    _settings = settings;
    for (size_t i = 0; i < submitted.size(); ++i)
    {
        if (!SameCapture(_probes[i].Source, submitted[i]))
        {
            _commonData->ResetHistory = true;
            ++_commonData->HistoryRevision;
            _probes[i].BakedFaces = 0;
            _probes[i].LastUpdate = 0;
            if (i == _activeProbe) _face = 0;
        }
        _probes[i].Source = submitted[i];
    }
    if (!_enabled) _face = 0;
    _enabled = true;
    if (_face == 0)
    {
        UINT visited = 0;
        while (visited < _probes.size())
        {
            const auto& probe = _probes[_activeProbe];
            const UINT interval = probe.Source.UpdateInterval;
            if (!probe.Ready || probe.LastUpdate == 0 ||
                (probe.Source.Implementation != 2 && _frameNumber - probe.LastUpdate >= interval)) break;
            _activeProbe = (_activeProbe + 1) % static_cast<UINT>(_probes.size());
            ++visited;
        }
        if (visited == _probes.size())
        {
            _planPrepared = true;
            return;
        }
    }
    auto& state = _probes[_activeProbe];
    CreateCaptureTextures(maximumResolution);
    _resolution = state.Source.Resolution;
    _mips = CubeMipCount(_resolution);
    auto& frame = _frames[_resources->CurrFrameConstantsIndex];
    for (UINT face = 0; face < 6; ++face) frame.Cameras->CopyData(face, FaceCamera(state.Source, face, _resolution));
    const bool sunShadow = (_resources->Lighting.SunShadowFlags & 1u) != 0;
    const auto shadowSettings = sunShadow ? ProbeSunShadow(*_resources, _sunShadow.GetWidth()) : GDX12ShadowConstants{};
    frame.Shadow->CopyData(0, shadowSettings);
    _capturePlanned = true;
    _endFace = (std::min)(6u, _face + state.Source.FacesPerFrame);
    const bool bake = state.Source.Implementation == 1 && state.BakedFaces != 63;
    const bool frozen = state.Source.Implementation == 2;
    _staticShadowsPlanned = bake || frozen;
    if (_staticShadowsPlanned)
    {
        if (!_staticLocalShadows)
        {
            // shortcut: baked receivers retain static-only shadows, use dynamic capture for moving shadow reception.
            _staticLocalShadows = std::make_unique<GDX12LocalShadowPass>(true);
            _staticLocalShadows->Setup(_resources, nullptr, _commonData);
            _staticLocalShadows->Initialize();
        }
        _staticLocalShadows->Prepare();
    }
    if (bake) CreateBakedTextures(state);
    const auto select = [&](const UINT slot, const Geometry geometry, const bool shadow, const UINT face, const bool baked)
    {
        auto& prepared = _views[slot];
        prepared.Filter = geometry;
        prepared.Face = face;
        prepared.Bake = baked;
        prepared.Selected = true;
        auto& view = prepared.Context;
        view.Stage = shadow ? GDX12GeometryStage::Shadow : GDX12GeometryStage::ReflectionCapture;
        view.ViewProjection = shadow ? Matrix(shadowSettings.ViewProj[0]).Transpose() :
            Matrix(FaceCamera(state.Source, face, _resolution).ViewProj).Transpose();
        view.Eye = state.Source.Position;
        view.CameraConstants = shadow ? 0 : frame.Cameras->GetElementAddress(face);
        view.Filter = geometry == StaticGeometry ? GDX12GeometryFilter::Static :
            (geometry == DynamicGeometry ? GDX12GeometryFilter::Dynamic : GDX12GeometryFilter::All);
        view.Key = shadow ? (4ull << 60) | (_activeProbe * 3ull + geometry) :
            (5ull << 60) | (static_cast<uint64_t>(_activeProbe) << 8) | (geometry * 6u + face);
        view.FrameSlot = _resources->CurrFrameConstantsIndex;
        view.DepthFormat = DXGI_FORMAT_D32_FLOAT;
        if (!shadow)
        {
            prepared.Lighting.SunShadow = &_sunShadow;
            prepared.Lighting.Irradiance = _inputs[0]->GetTexture();
            prepared.Lighting.Prefiltered = _inputs[1]->GetTexture();
            prepared.Lighting.BRDF = _inputs[2]->GetTexture();
            prepared.Lighting.LocalShadows = geometry == StaticGeometry ?
                _staticLocalShadows->GetOutput(0)->GetTexture() : _inputs[4]->GetTexture();
            prepared.Lighting.ShadowConstants = frame.Shadow->GetElementAddress(0);
            view.CaptureLighting = &prepared.Lighting;
            view.ColorTargetCount = 1;
            view.ColorFormats[0] = baked ? state.BakedColor->GetFormat() : _raw.GetFormat();
        }
    };
    if (bake)
    {
        if (sunShadow) select(0, StaticGeometry, true, 0, false);
        for (UINT face = _face; face < _endFace; ++face)
            if (!(state.BakedFaces & (1u << face))) select(1 + face, StaticGeometry, false, face, true);
    }
    if (sunShadow) select(7, frozen ? StaticGeometry : AllGeometry, true, 0, false);
    for (UINT face = _face; face < _endFace; ++face)
        select(8 + face, frozen ? StaticGeometry : state.Source.Implementation == 1 ? DynamicGeometry : AllGeometry,
            false, face, false);
    assert(_face < _endFace && _endFace <= 6);
    for (const auto& view : _views)
    {
        if (!view.Selected || view.Context.Stage != GDX12GeometryStage::ReflectionCapture) continue;
        assert(view.Face >= _face && view.Face < _endFace);
        assert(view.Context.CaptureLighting == &view.Lighting && view.Context.CameraConstants != 0);
        if (view.Filter == DynamicGeometry)
            assert((state.BakedFaces & (1u << view.Face)) || _views[1 + view.Face].Selected);
    }
    _planPrepared = true;
}

void GDX12ReflectionProbePass::BeginGeometry(GDX12CommandList* commands)
{
    // Standalone users may execute without the pipeline's CPU preparation phase.
    Prepare();
    if (_publishedNeedsTransition)
    {
        commands->ResourceBarrier({ _published.GetResource()->GetSRVBarrier() });
        _publishedNeedsTransition = false;
    }
    if (!_capturePlanned) return;
    commands->SetDescriptorHeaps({ _resources->SRV_UAV_Heap.get() });
    if (_sunShadowNeedsClear)
    {
        commands->ResourceBarrier({ _sunShadow.GetResource()->GetDepthWriteBarrier() });
        commands->GetCommandList()->ClearDepthStencilView(_sunDSV[0]->CPUHandle, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
        _sunShadowNeedsClear = false;
    }
    if (_staticShadowsPlanned) _staticLocalShadows->Execute(commands);
}

void GDX12ReflectionProbePass::EndGeometry(GDX12CommandList* commands)
{
    if (_capturePlanned)
    {
        auto& state = _probes[_activeProbe];
        for (const auto& view : _views)
        {
            if (!view.Selected || view.Context.Stage != GDX12GeometryStage::ReflectionCapture) continue;
            if (view.Bake)
            {
                state.BakedFaces |= 1u << view.Face;
                ++_statistics.BakedFaces;
            }
            else ++_statistics.CapturedFaces;
        }
        _face = _endFace;
        if (_face == 6)
        {
            FilterAndPublish(commands, _activeProbe);
            _activeProbe = (_activeProbe + 1) % static_cast<UINT>(_probes.size());
            _face = 0;
        }
    }
    WriteProbeData(commands, _enabled);
    _planPrepared = false;
}

void GDX12ReflectionProbePass::ClearDenendencies()
{
    GDX12RenderPass::ClearDenendencies();
    if (_staticLocalShadows) _staticLocalShadows->ClearDenendencies();
    _staticLocalShadows.reset();
    _frames.clear();
    _probes.clear();
    _views = {};
    _planPrepared = _capturePlanned = _staticShadowsPlanned = false;
}

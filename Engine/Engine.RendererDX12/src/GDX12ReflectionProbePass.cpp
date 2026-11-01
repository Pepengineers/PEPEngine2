#include "Engine.RendererDX12/RenderPasses/GDX12ReflectionProbePass.h"
#include <algorithm>
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
            a.Implementation == b.Implementation && a.Resolution == b.Resolution;
    }

    GDX12CameraConstants FaceCamera(const GDX12ReflectionProbe& probe, UINT face, UINT size)
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

    GDX12ShadowConstants ProbeSunShadow(const GDX12DeviceResources& resources)
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
        result.TexelWorldSize.x = (std::max)(maximum.x - minimum.x, maximum.y - minimum.y) / 1024;
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
    capture.Constants.push_back(1);
    capture.NumSingleCBVSlots = 3;
    capture.NumSingleSRVSlots = 10;
    capture.SRVRanges.emplace_back(Texture2D_RangeLength);
    capture.StaticSamplers = GetStaticSamplers();
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
    const auto compute = [&](const wchar_t* path, const char* entry, D3D_SHADER_MACRO* defines,
        GDX12RootSignature* root, ComPtr<ID3D12PipelineState>& pipeline)
    {
        auto shader = compiler.CompileShader(_resources->Device, path, defines, entry, "cs");
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = root->GetRootSignature().Get();
        desc.CS = { shader->GetBufferPointer(), shader->GetBufferSize() };
        ThrowIfFailed(_resources->Device->GetDevice()->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline)));
    };
    D3D_SHADER_MACRO culling[] = { { "PROBE_CULLING", "1" }, { nullptr, nullptr } };
    compute(SHADERS_FOLDER "ReflectionCapture.hlsl", "CullCS", culling, _cullRoot.get(), _cullPSO);
    compute(SHADERS_FOLDER "ReflectionCapture.hlsl", "ClearCS", culling, _cullRoot.get(), _clearPSO);
    D3D_SHADER_MACRO filtering[] = { { "LOCAL_PROBE_FILTER", "1" }, { nullptr, nullptr } };
    compute(SHADERS_FOLDER "EnvironmentIBL.hlsl", "PrefilterCS", filtering, _filterRoot.get(), _filterPSO);
    D3D12_INDIRECT_ARGUMENT_DESC arguments[2] = {};
    arguments[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
    arguments[0].Constant.Num32BitValuesToSet = 1;
    arguments[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
    D3D12_COMMAND_SIGNATURE_DESC signature = {};
    signature.ByteStride = sizeof(GDX12IndirectDrawArgs);
    signature.NumArgumentDescs = 2;
    signature.pArgumentDescs = arguments;
    ThrowIfFailed(_resources->Device->GetDevice()->CreateCommandSignature(&signature,
        _captureRoot->GetRootSignature().Get(), IID_PPV_ARGS(&_signature)));
    _frames.resize(_resources->FrameConstants.size());
    for (auto& frame : _frames)
    {
        frame.Cameras = std::make_unique<GDX12UploadBuffer<GDX12CameraConstants>>(_resources->Device, 6);
        frame.Shadow = std::make_unique<GDX12UploadBuffer<GDX12ShadowConstants>>(_resources->Device, 1);
        frame.Counter = std::make_unique<GDX12UploadBuffer<UINT>>(_resources->Device, 1, EBufferType::Default, false);
        frame.Counter->CreateUAV(_resources->SRV_UAV_Heap.get(), _resources->SRV_UAV_Heap->GetAvailableIndex(ConstantsResources));
    }
}

void GDX12ReflectionProbePass::CreateTexture(GDX12Texture& texture, UINT size, UINT slices, UINT mips,
    D3D12_RESOURCE_FLAGS flags, bool depth)
{
    const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    const DXGI_FORMAT format = depth ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_R16G16B16A16_FLOAT;
    const auto resource = CD3DX12_RESOURCE_DESC::Tex2D(format, size, size,
        static_cast<UINT16>(slices), static_cast<UINT16>(mips), 1, 0, flags);
    D3D12_CLEAR_VALUE clear = {};
    clear.Format = depth ? DXGI_FORMAT_D32_FLOAT : format;
    GDX12TextureDesc desc;
    const bool clearable = (flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) != 0;
    ThrowIfFailed(_resources->Device->GetDevice()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
        &resource, D3D12_RESOURCE_STATE_COMMON, clearable ? &clear : nullptr, IID_PPV_ARGS(&desc.ExternalResource)));
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

void GDX12ReflectionProbePass::CreatePublishedTexture(GDX12CommandList* commands, UINT count, UINT resolution)
{
    _resources->Device->GetCommandQueue()->Flush();
    _publishedResolution = resolution;
    _publishedMips = CubeMipCount(resolution);
    CreateTexture(_published, resolution, count * 6, _publishedMips, D3D12_RESOURCE_FLAG_NONE, false);
    _published.GetResource()->D3DResource->SetName(L"Reflection published radiance");
    commands->ResourceBarrier({ _published.GetResource()->GetSRVBarrier() });
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
    if (!_sunShadow.IsInitialized())
    {
        CreateTexture(_sunShadow, 1024, 1, 1, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, true);
        _sunShadow.GetResource()->D3DResource->SetName(L"Reflection sun shadow");
        CreateFaceViews(_sunShadow, _sunDSV, true);
    }
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

void GDX12ReflectionProbePass::Cull(GDX12CommandList* commands, UINT camera, Geometry geometry, bool shadow)
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
    commands->SetComputeSRV(0, constants->InstanceCache->GetSRV()->GPUHandle);
    commands->SetComputeSRV(1, _resources->IndirectCommandsCache->GetSRV()->GPUHandle);
    commands->SetComputeSRV(2, constants->MaterialCache->GetSRV()->GPUHandle);
    if (count) commands->Dispatch((count + 63) / 64, 1, 1);
    commands->ResourceBarrier({ frame.Commands->GetResource().GetIndirectArgsBarrier(), frame.Counter->GetResource().GetIndirectArgsBarrier() });
}

void GDX12ReflectionProbePass::BindCapture(GDX12CommandList* commands, UINT face, GDX12Texture* localShadows)
{
    auto& frame = _frames[_resources->CurrFrameConstantsIndex];
    auto& constants = _resources->FrameConstants[_resources->CurrFrameConstantsIndex];
    commands->SetGraphicsRootSignature(_captureRoot.get());
    commands->SetGraphicsRootConstantBufferView(1, constants->MainCB->GetElementAddress(0));
    commands->SetGraphicsRootConstantBufferView(2, frame.Cameras->GetElementAddress(face));
    commands->SetGraphicsRootConstantBufferView(3, frame.Shadow->GetElementAddress(0));
    commands->SetGraphicsSRV(0, constants->MaterialCache->GetSRV()->GPUHandle);
    commands->SetGraphicsSRV(1, constants->TransformCache->GetSRV()->GPUHandle);
    commands->SetGraphicsSRV(2, constants->InstanceCache->GetSRV()->GPUHandle);
    commands->SetGraphicsSRV(3, _sunShadow.GetSRV()->GPUHandle);
    for (UINT i = 0; i < 3; ++i) commands->SetGraphicsSRV(4 + i, _inputs[i]->GetTexture()->GetSRV()->GPUHandle);
    commands->SetGraphicsSRV(7, constants->LocalLightCache->GetSRV()->GPUHandle);
    commands->SetGraphicsSRV(8, localShadows->GetSRV()->GPUHandle);
    commands->SetGraphicsSRV(9, _inputs[3]->GetTexture()->GetSRV()->GPUHandle);
    commands->SetGraphicsSRV(10, _resources->SRV_UAV_Heap->GetGPUHandle(Texture2D_StartIndex));
    commands->SetGeometryBuffer(_resources->GeometryBuffer.get());
    commands->SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void GDX12ReflectionProbePass::RenderShadow(GDX12CommandList* commands, Geometry geometry)
{
    if ((_resources->Lighting.SunShadowFlags & 1u) == 0)
    {
        commands->ResourceBarrier({ _sunShadow.GetResource()->GetSRVBarrier() });
        return;
    }
    Cull(commands, 0, geometry, true);
    auto& constants = _resources->FrameConstants[_resources->CurrFrameConstantsIndex];
    auto& frame = _frames[_resources->CurrFrameConstantsIndex];
    commands->SetGraphicsRootSignature(_captureRoot.get());
    commands->SetGraphicsRootConstantBufferView(3, frame.Shadow->GetElementAddress(0));
    commands->SetGraphicsSRV(0, constants->MaterialCache->GetSRV()->GPUHandle);
    commands->SetGraphicsSRV(1, constants->TransformCache->GetSRV()->GPUHandle);
    commands->SetGraphicsSRV(2, constants->InstanceCache->GetSRV()->GPUHandle);
    commands->SetGraphicsSRV(10, _resources->SRV_UAV_Heap->GetGPUHandle(Texture2D_StartIndex));
    commands->SetGeometryBuffer(_resources->GeometryBuffer.get());
    commands->SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commands->SetPipelineState(_shadowPSO);
    commands->ResourceBarrier({ _sunShadow.GetResource()->GetDepthWriteBarrier() });
    const auto dsv = _sunDSV[0]->CPUHandle;
    commands->GetCommandList()->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
    commands->GetCommandList()->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
    commands->SetViewport({ 0, 0, 1024, 1024, 0, 1 });
    commands->SetScissorRect({ 0, 0, 1024, 1024 });
    const UINT count = _resources->IndirectCommandsCache->GetElementCount();
    if (count) commands->ExecuteIndirect(_signature.Get(), count, frame.Commands->GetResource().D3DResource.Get(), 0,
        frame.Counter->GetResource().D3DResource.Get(), 0);
    commands->ResourceBarrier({ _sunShadow.GetResource()->GetSRVBarrier() });
}

void GDX12ReflectionProbePass::RenderFace(GDX12CommandList* commands, UINT probe, UINT face, Geometry geometry, bool bake)
{
    auto& state = _probes[probe];
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
    BindCapture(commands, face, bake ? _staticLocalShadows->GetOutput(0)->GetTexture() : _inputs[4]->GetTexture());
    commands->ResourceBarrier({ color.GetResource()->GetRenderTargetBarrier(), depth.GetResource()->GetDepthWriteBarrier() });
    commands->GetCommandList()->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    commands->SetViewport({ 0, 0, static_cast<float>(_resolution), static_cast<float>(_resolution), 0, 1 });
    commands->SetScissorRect({ 0, 0, static_cast<LONG>(_resolution), static_cast<LONG>(_resolution) });
    if (bake || geometry == AllGeometry)
    {
        const float clear[] = { 0, 0, 0, 0 };
        commands->GetCommandList()->ClearRenderTargetView(rtv, clear, 0, nullptr);
        commands->GetCommandList()->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0, 0, 0, nullptr);
        commands->SetPipelineState(_skyPSO);
        commands->DrawInstanced(3, 1, 0, 0);
    }
    commands->SetPipelineState(_capturePSO);
    auto& frame = _frames[_resources->CurrFrameConstantsIndex];
    const UINT count = _resources->IndirectCommandsCache->GetElementCount();
    if (count) commands->ExecuteIndirect(_signature.Get(), count, frame.Commands->GetResource().D3DResource.Get(), 0,
        frame.Counter->GetResource().D3DResource.Get(), 0);
}

void GDX12ReflectionProbePass::FilterAndPublish(GDX12CommandList* commands, UINT probe)
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

void GDX12ReflectionProbePass::WriteProbeData(bool enabled)
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
        frame->ReflectionProbes->CopyData(i, data);
    }
}

void GDX12ReflectionProbePass::Execute(GDX12CommandList* commands)
{
    ++_frameNumber;
    _statistics.CapturedFaces = _statistics.BakedFaces = _statistics.PublishedProbes = 0;
    const auto settings = GetReflectionSettings();
    if (!settings.Enabled || _resources->ReflectionProbes.empty())
    {
        WriteProbeData(false);
        if (_enabled)
        {
            _commonData->ResetHistory = true;
            ++_commonData->HistoryRevision;
        }
        _enabled = false;
        _face = 0;
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
        CreatePublishedTexture(commands, static_cast<UINT>(submitted.size()), maximumResolution);
    const bool invalidate = _staticRevision != _resources->ReflectionStaticRevision ||
        _environmentRevision != _resources->ReflectionEnvironmentRevision;
    if (invalidate)
    {
        _face = 0;
        for (auto& probe : _probes)
        {
            probe.BakedFaces = 0;
            probe.LastUpdate = 0;
        }
        if (_staticLocalShadows) _staticLocalShadows->InvalidateCache();
    }
    _staticRevision = _resources->ReflectionStaticRevision;
    _environmentRevision = _resources->ReflectionEnvironmentRevision;
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
            if (!probe.Ready || probe.LastUpdate == 0 || _frameNumber - probe.LastUpdate >= interval) break;
            _activeProbe = (_activeProbe + 1) % static_cast<UINT>(_probes.size());
            ++visited;
        }
        if (visited == _probes.size())
        {
            WriteProbeData(true);
            return;
        }
    }
    auto& state = _probes[_activeProbe];
    CreateCaptureTextures(maximumResolution);
    _resolution = state.Source.Resolution;
    _mips = CubeMipCount(_resolution);
    auto& frame = _frames[_resources->CurrFrameConstantsIndex];
    for (UINT face = 0; face < 6; ++face) frame.Cameras->CopyData(face, FaceCamera(state.Source, face, _resolution));
    frame.Shadow->CopyData(0, ProbeSunShadow(*_resources));
    commands->BeginPixEvent("Local reflection capture and GGX", Colors::MediumPurple);
    commands->SetDescriptorHeaps({ _resources->SRV_UAV_Heap.get() });
    const UINT endFace = (std::min)(6u, _face + state.Source.FacesPerFrame);
    if (state.Source.Implementation == 1 && state.BakedFaces != 63)
    {
        CreateBakedTextures(state);
        if (!_staticLocalShadows)
        {
            // shortcut: baked receivers retain static-only shadows, use dynamic capture for moving shadow reception.
            _staticLocalShadows = std::make_unique<GDX12LocalShadowPass>(true);
            _staticLocalShadows->Setup(_resources, nullptr, _commonData);
            _staticLocalShadows->Initialize();
        }
        _staticLocalShadows->Execute(commands);
        RenderShadow(commands, StaticGeometry);
        for (UINT face = _face; face < endFace; ++face)
        {
            if (state.BakedFaces & (1u << face)) continue;
            RenderFace(commands, _activeProbe, face, StaticGeometry, true);
            state.BakedFaces |= 1u << face;
            ++_statistics.BakedFaces;
        }
    }
    RenderShadow(commands, AllGeometry);
    for (; _face < endFace; ++_face)
    {
        RenderFace(commands, _activeProbe, _face, state.Source.Implementation == 1 ? DynamicGeometry : AllGeometry, false);
        ++_statistics.CapturedFaces;
    }
    if (_face == 6)
    {
        FilterAndPublish(commands, _activeProbe);
        _activeProbe = (_activeProbe + 1) % static_cast<UINT>(_probes.size());
        _face = 0;
    }
    commands->EndPixEvent();
    WriteProbeData(true);
}

void GDX12ReflectionProbePass::ClearDenendencies()
{
    GDX12RenderPass::ClearDenendencies();
    if (_staticLocalShadows) _staticLocalShadows->ClearDenendencies();
    _staticLocalShadows.reset();
    _frames.clear();
    _probes.clear();
}

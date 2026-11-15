#include "SceneSunShadows.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include "Engine.RendererDX12/GDX12ShaderCompiler.h"
#include "Engine.RendererDX12/GDX12ShadowCascades.h"
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include <cstring>
#include <type_traits>

namespace Meta
{
SceneSunShadows::SceneSunShadows(GDX12DeviceResources& r, RenderPipelineCommonData& common)
    : _resources(r), _common(common), _slots(r.FrameConstants.size())
{
    GDX12RootSignatureDesc graphics;
    graphics.InstancedGeometry = true;
    graphics.Constants = {2};
    graphics.NumSingleCBVSlots = 1;
    graphics.NumSingleSRVSlots = 3;
    graphics.StaticSamplers = GetStaticSamplers();
    graphics.SRVRanges.emplace_back(Texture2D_RangeLength);
    _graphicsRoot = std::make_unique<GDX12RootSignature>(r.Device, graphics);
    GDX12RootSignatureDesc compute;
    compute.NumSingleCBVSlots = 1;
    compute.NumSingleSRVSlots = 3;
    compute.NumSingleUAVSlots = 5;
    _computeRoot = std::make_unique<GDX12RootSignature>(r.Device, compute);
    D3D_SHADER_MACRO defines[] = {{"SHADOW_CULLING", "1"}, {nullptr, nullptr}};
    _cull = GDX12RenderHelpers::CreateComputePipeline(r.Device, _computeRoot.get(), SHADERS_FOLDER "ShadowPass.hlsl", "CS", defines);
    _clear = GDX12RenderHelpers::CreateComputePipeline(r.Device, _computeRoot.get(), SHADERS_FOLDER "ShadowPass.hlsl", "ClearCS", defines);
    auto& compiler = GDX12ShaderCompiler::GetInstance();
    const D3D_SHADER_MACRO unbatched[] = {{"SHADOW_UNBATCHED", "1"}, {nullptr, nullptr}};
    auto vs = compiler.CompileShader(r.Device, SHADERS_FOLDER "ShadowPass.hlsl", unbatched, "VS", "vs");
    auto ps = compiler.CompileShader(r.Device, SHADERS_FOLDER "ShadowPass.hlsl", nullptr, "PS", "ps");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};
    p.pRootSignature = _graphicsRoot->GetRootSignature().Get();
    p.InputLayout = {r.InputLayouts.at("Default").data(), UINT(r.InputLayouts.at("Default").size())};
    p.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    p.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    p.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    p.RasterizerState.FrontCounterClockwise = TRUE;
    p.RasterizerState.DepthBias = 500;
    p.RasterizerState.SlopeScaledDepthBias = 1.5f;
    p.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    p.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    p.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    p.SampleMask = UINT_MAX;
    p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    p.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    p.SampleDesc.Count = 1;
    ThrowIfFailed(r.Device->GetDevice()->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&_draw)));
    _signature = GDX12RenderHelpers::CreateIndexedDrawSignature(r.Device, _graphicsRoot.get());
}

void SceneSunShadows::Freeze(UINT slot, bool castersRebuilt, const std::vector<BoundingBox>& changedCasters,
    const std::vector<GDX12RenderPrimitive>& grass)
{
    for (auto& cached : _slots)
    {
        if (castersRebuilt) { cached.BoundsReady = false; cached.DirtyCascades = 0xf; }
        for (const auto& bounds : changedCasters)
        {
            // Keep the fit conservative between topology rebuilds without rescanning static Bistro.
            if (cached.HasBounds) BoundingBox::CreateMerged(cached.Bounds, cached.Bounds, bounds);
            else { cached.Bounds = bounds; cached.HasBounds = true; }
            for (UINT cascade = 0; cascade < cached.Settings.CascadeCount; ++cascade)
                if (GDX12LocalShadowFrustum(cached.Settings.ViewProj[cascade]).Intersects(bounds))
                    cached.DirtyCascades |= 1u << cascade;
        }
    }
    auto& s = _slots.at(slot);
    if (!s.BoundsReady)
    {
        s.HasBounds = false;
        const auto& frame = *_resources.FrameConstants.at(slot);
        const auto* instances = frame.InstanceCache->GetCPUData();
        const auto* materials = frame.MaterialCache->GetCPUData();
        const auto* commands = _resources.IndirectCommandsCache->GetCPUData();
        for (UINT i = 0; i < _resources.IndirectCommandsCache->GetElementCount(); ++i)
        {
            if (!commands[i].InstanceCount) continue;
            const auto& instance = instances[commands[i].InstanceID];
            const auto& material = materials[instance.MaterialIndex];
            if (!instance.CastsShadows || material.RenderLayer == 2 || material.ReflectionProbePreview) continue;
            const BoundingBox bounds(instance.BoundingBoxCenter, instance.BoundingBoxExtents);
            if (s.HasBounds) BoundingBox::CreateMerged(s.Bounds, s.Bounds, bounds);
            else s.Bounds = bounds;
            s.HasBounds = true;
        }
        s.BoundsReady = true;
    }
    BoundingBox bounds = s.Bounds;
    bool hasBounds = s.HasBounds;
    uint64_t grassRevision = 14695981039346656037ull;
    for (const auto& primitive : grass)
    {
        if (!primitive.Visible || !primitive.CastsShadows || !primitive.InstanceCount) continue;
        if (hasBounds) BoundingBox::CreateMerged(bounds, bounds, primitive.Bounds);
        else bounds = primitive.Bounds;
        hasBounds = true;
        grassRevision = (grassRevision ^ primitive.Revision()) * 1099511628211ull;
    }
    const bool grassChanged = grassRevision != s.GrassRevision;
    s.GrassRevision = grassRevision;
    _resources.ShadowSettings = BuildShadowCascades(_common.CameraView.Invert(),
        XMConvertToRadians(_common.ActiveCameraFOV), float(_common.DownscaledWidth) / _common.DownscaledHeight,
        _common.ActiveCameraNearPlane, _common.ActiveCameraFarPlane, Vector3(_resources.Lighting.LightDirection),
        hasBounds ? &bounds : nullptr, _resources.ShadowQuality.SunResolution, _resources.ShadowQuality.SunCascades);
    const UINT flags = _resources.Lighting.SunShadowFlags;
    const bool fullUpdate = (flags & 2u) || !s.Rendered || grassChanged ||
        s.Resolution != _resources.ShadowQuality.SunResolution || s.Revision != _resources.Lighting.SunShadowRevision ||
        std::memcmp(&s.Settings, &_resources.ShadowSettings, sizeof(s.Settings)) != 0;
    const UINT cascades = (1u << _resources.ShadowSettings.CascadeCount) - 1;
    s.Update = (flags & 1u) ? (fullUpdate ? cascades : s.DirtyCascades & cascades) : 0;
}

void SceneSunShadows::Append(Graph& graph, std::vector<ID3D12Resource*>& bindings, UINT slot,
    const std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 4>& scene, UINT draws,
    GDX12GeometryBuffer::RecordingBinding geometry,
    const std::function<void(const GeometryTarget&, Graph&, std::vector<ID3D12Resource*>&)>& appendGrass)
{
    auto& s = _slots.at(slot);
    auto& work = s.Work;
    work.Commands.clear();
    const UINT resolution = _resources.ShadowQuality.SunResolution;
    const auto resource = [&](ID3D12Resource* value, const char* name) {
        return NativeWork::ResourceFor(graph, bindings, value, name);
    };
    if (s.Resolution != resolution)
    {
        GDX12TextureDesc texture;
        texture.Width = texture.Height = (std::max)(1u, resolution * 2);
        texture.Format = texture.DSVDesc.Format = DXGI_FORMAT_D32_FLOAT;
        texture.SRVDesc.Format = DXGI_FORMAT_R32_FLOAT;
        texture.SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        texture.SRVDesc.Texture2D.MipLevels = 1;
        texture.SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        texture.SRV_UAV_Heap = _resources.SRV_UAV_Heap.get();
        texture.SRVHeapIndex = s.Atlas.IsInitialized() ? s.Atlas.GetSRV()->HeapIndex : texture.SRV_UAV_Heap->GetAvailableIndex(105000);
        texture.CreateDSV = true;
        texture.DSVHeap = _resources.DSVHeap.get();
        texture.DSVHeapIndex = s.Atlas.IsInitialized() ? s.Atlas.GetDSV()->HeapIndex : texture.DSVHeap->GetAvailableIndex();
        texture.DSVDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        texture.ClearValue = {1, 0, 0, 0};
        const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32_TYPELESS, texture.Width, texture.Height,
            1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
        D3D12_CLEAR_VALUE clear{};
        clear.Format = DXGI_FORMAT_D32_FLOAT;
        clear.DepthStencil.Depth = 1;
        texture.ExternalResource = _resources.Device->CreateResource(D3D12_HEAP_TYPE_DEFAULT, desc, D3D12_RESOURCE_STATE_COMMON, &clear);
        s.Atlas.Initialize(texture);
        s.Resolution = resolution;
        s.Rendered = false;
    }
    const Id atlas = resource(s.Atlas.GetResource()->D3DResource.Get(), "sun shadow atlas");
    const auto dsv = s.Atlas.GetDSV()->CPUHandle;
    if (!s.Update || !resolution)
    {
        if (!s.Rendered)
        {
            work.Add(graph, "initialize sun shadow atlas", {{atlas, Mode::Write, D3D12_RESOURCE_STATE_DEPTH_WRITE}},
                [=](auto* cmd) { cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr); });
            s.Rendered = true;
        }
        return;
    }
    const auto buffer = [&](auto& target, UINT count) {
        using Buffer = typename std::remove_reference_t<decltype(target)>::element_type;
        if (!target || target->GetElementCount() < count)
        {
            target = std::make_unique<Buffer>(_resources.Device, count, EBufferType::Default, false);
            target->CreateUAV(_resources.SRV_UAV_Heap.get(), _resources.SRV_UAV_Heap->GetAvailableIndex(105000));
        }
    };
    buffer(s.Counters, 4);
    for (auto& commands : s.Commands) buffer(commands, (std::max)(draws, 1u));
    const Id counters = resource(s.Counters->GetResource().D3DResource.Get(), "sun visible counts");
    const auto cbv = _resources.FrameConstants.at(slot)->ShadowCB->GetElementAddress(0);
    const Id constants = resource(_resources.FrameConstants.at(slot)->ShadowCB->GetResource().D3DResource.Get(), "sun cascade constants");
    auto* heap = _resources.SRV_UAV_Heap->GetHeap().Get();
    auto* compute = _computeRoot.get();
    auto* clear = _clear.Get();
    auto* cull = _cull.Get();
    const auto countsUav = s.Counters->GetUAV()->GPUHandle;
    work.Add(graph, "sun reset visible counts", {{counters, Mode::Write, UnorderedAccess}}, [=](auto* cmd) {
        cmd->SetDescriptorHeaps(1, &heap);
        cmd->SetComputeRootSignature(compute->GetRootSignature().Get());
        cmd->SetPipelineState(clear);
        cmd->SetComputeRootDescriptorTable(compute->GetURootParamIndex(4), countsUav);
        cmd->Dispatch(1, 1, 1);
    });
    std::vector<Access> culling{{0, Mode::Read, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE},
        {1, Mode::Read, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE}, {2, Mode::Read, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE},
        {constants, Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ}, {counters, Mode::ReadWrite, UnorderedAccess}};
    std::vector<Access> rendering{{0, Mode::Read, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE},
        {2, Mode::Read, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE}, {3, Mode::Read, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE},
        {21, Mode::Read, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE},
        {22, Mode::Read, D3D12_RESOURCE_STATE_INDEX_BUFFER}, {constants, Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ},
        {counters, Mode::Read, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT}, {atlas, Mode::Write, D3D12_RESOURCE_STATE_DEPTH_WRITE}};
    std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 4> uavs{};
    std::array<ID3D12Resource*, 4> commands{};
    for (UINT i = 0; i < 4; ++i)
    {
        commands[i] = s.Commands[i]->GetResource().D3DResource.Get();
        const Id id = resource(commands[i], "sun visible commands");
        uavs[i] = s.Commands[i]->GetUAV()->GPUHandle;
        culling.push_back({id, Mode::Write, UnorderedAccess});
        rendering.push_back({id, Mode::Read, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT});
    }
    const UINT cascadeCount = _resources.ShadowSettings.CascadeCount;
    const UINT update = s.Update;
    work.Add(graph, "sun cull " + std::to_string(cascadeCount) + " cascades", std::move(culling), [=](auto* cmd) {
        cmd->SetDescriptorHeaps(1, &heap);
        cmd->SetComputeRootSignature(compute->GetRootSignature().Get());
        cmd->SetPipelineState(cull);
        cmd->SetComputeRootConstantBufferView(compute->GetBRootParamIndex(0), cbv);
        for (UINT i = 0; i < 3; ++i) cmd->SetComputeRootDescriptorTable(compute->GetTRootParamIndex(i), scene[i]);
        for (UINT i = 0; i < 4; ++i) cmd->SetComputeRootDescriptorTable(compute->GetURootParamIndex(i), uavs[i]);
        cmd->SetComputeRootDescriptorTable(compute->GetURootParamIndex(4), countsUav);
        if (draws) cmd->Dispatch((draws + 63) / 64, 1, 1);
    });
    auto* graphics = _graphicsRoot.get();
    auto* draw = _draw.Get();
    auto* signature = _signature.Get();
    auto* counts = s.Counters->GetResource().D3DResource.Get();
    const auto textures = _resources.SRV_UAV_Heap->GetGPUHandle(Texture2D_StartIndex);
    work.Add(graph, "sun atlas " + std::to_string(cascadeCount) + " cascades ExecuteIndirect", std::move(rendering), [=](auto* cmd) {
        cmd->SetDescriptorHeaps(1, &heap);
        cmd->SetGraphicsRootSignature(graphics->GetRootSignature().Get());
        cmd->SetPipelineState(draw);
        cmd->SetGraphicsRootConstantBufferView(graphics->GetBRootParamIndex(1), cbv);
        const UINT inputs[] = {2, 3, 0};
        for (UINT i = 0; i < 3; ++i) cmd->SetGraphicsRootDescriptorTable(graphics->GetTRootParamIndex(i), scene[inputs[i]]);
        cmd->SetGraphicsRootDescriptorTable(graphics->GetTRootParamIndex(3), textures);
        const auto root = graphics->GetGeometryRootParamIndex();
        cmd->SetGraphicsRootShaderResourceView(root, geometry.Vertices.BufferLocation);
        cmd->SetGraphicsRootShaderResourceView(root + 1, geometry.Vertices.BufferLocation);
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmd->IASetVertexBuffers(0, 1, &geometry.Vertices);
        cmd->IASetIndexBuffer(&geometry.Indices);
        cmd->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
        for (UINT cascade = 0; cascade < cascadeCount; ++cascade)
        {
            if (!(update & (1u << cascade))) continue;
            const auto region = SunShadowRegion(cascade, resolution);
            const D3D12_VIEWPORT viewport{float(region.X), float(region.Y), float(region.Width), float(region.Height), 0, 1};
            const D3D12_RECT rect{LONG(region.X), LONG(region.Y), LONG(region.X + region.Width), LONG(region.Y + region.Height)};
            cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 1, &rect);
            cmd->RSSetViewports(1, &viewport);
            cmd->RSSetScissorRects(1, &rect);
            cmd->SetGraphicsRoot32BitConstant(0, 0, 0);
            cmd->SetGraphicsRoot32BitConstant(0, cascade, 1);
            if (draws) cmd->ExecuteIndirect(signature, draws, commands[cascade], 0, counts, cascade * sizeof(UINT));
        }
    });
    if (appendGrass)
    {
        auto& camera = *_resources.FrameConstants.at(slot)->CameraCB;
        const Id cameraId = resource(camera.GetResource().D3DResource.Get(), "sun grass camera constants");
        for (UINT cascade = 0; cascade < cascadeCount; ++cascade)
        {
            if (!(update & (1u << cascade))) continue;
            const auto region = SunShadowRegion(cascade, resolution);
            GeometryTarget target;
            target.View = {GDX12GeometryStage::Shadow, Matrix(_resources.ShadowSettings.ViewProj[cascade]).Transpose(),
                _common.CameraView.Invert().Translation(), camera.GetElementAddress(_common.ActiveCameraCBufferIndex)};
            target.View.Key = (4ull << 60) | cascade;
            target.View.FrameSlot = slot;
            target.View.DepthFormat = DXGI_FORMAT_D32_FLOAT;
            target.Depth = atlas;
            target.Camera = cameraId;
            target.Dsv = dsv;
            target.X = region.X;
            target.Y = region.Y;
            target.Width = region.Width;
            target.Height = region.Height;
            appendGrass(target, graph, bindings);
        }
    }
    s.Settings = _resources.ShadowSettings;
    s.Revision = _resources.Lighting.SunShadowRevision;
    s.Rendered = true;
    s.DirtyCascades &= ~update;
}
}

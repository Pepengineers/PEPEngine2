#include "SceneLocalShadows.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include "Engine.RendererDX12/GDX12ShaderCompiler.h"
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace Meta
{
SceneLocalShadows::SceneLocalShadows(GDX12DeviceResources& r, RenderPipelineCommonData& common)
    : _resources(r), _common(common), _slots(r.FrameConstants.size())
{
    GDX12RootSignatureDesc root;
    root.Constants = {2};
    root.NumSingleSRVSlots = 4;
    root.InstancedGeometry = true;
    root.StaticSamplers = GetStaticSamplers();
    root.SRVRanges.emplace_back(Texture2D_RangeLength);
    _root = std::make_unique<GDX12RootSignature>(r.Device, root);
    _signature = GDX12RenderHelpers::CreateIndexedDrawSignature(r.Device, _root.get());
    auto& compiler = GDX12ShaderCompiler::GetInstance();
    const D3D_SHADER_MACRO defines[] = {{"SHADOW_UNBATCHED", "1"}, {nullptr, nullptr}};
    auto vs = compiler.CompileShader(r.Device, SHADERS_FOLDER "LocalShadowPass.hlsl", defines, "VS", "vs");
    auto ps = compiler.CompileShader(r.Device, SHADERS_FOLDER "LocalShadowPass.hlsl", nullptr, "PS", "ps");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};
    p.pRootSignature = _root->GetRootSignature().Get();
    p.InputLayout = {r.InputLayouts.at("Default").data(), UINT(r.InputLayouts.at("Default").size())};
    p.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    p.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    p.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    p.RasterizerState.FrontCounterClockwise = TRUE;
    p.RasterizerState.DepthBias = 250;
    p.RasterizerState.SlopeScaledDepthBias = 1.5f;
    p.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    p.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    p.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    p.SampleMask = UINT_MAX;
    p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    p.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    p.SampleDesc.Count = 1;
    ThrowIfFailed(r.Device->GetDevice()->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&_draw)));
    for (auto& frame : _slots)
    {
        frame.Cull.Owner = this;
        frame.Cull.Frame = frame.Pack.Frame = &frame;
    }
}

SceneLocalShadows::~SceneLocalShadows()
{
    for (auto& frame : _slots)
        try { frame.Ready.Complete(); } catch (...) {}
}

UINT SceneLocalShadows::BuildNode(UINT first, UINT count)
{
    const UINT index = UINT(_nodes.size());
    _nodes.emplace_back();
    auto bounds = _casters[first].Bounds;
    for (UINT i = first + 1; i < first + count; ++i) BoundingBox::CreateMerged(bounds, bounds, _casters[i].Bounds);
    if (count <= 32) _nodes[index] = {bounds, first, count};
    else
    {
        const auto& e = bounds.Extents;
        const UINT axis = e.x >= e.y && e.x >= e.z ? 0 : e.y >= e.z ? 1 : 2;
        const UINT half = count / 2;
        const auto coordinate = [axis](const Caster& value) {
            return axis == 0 ? value.Bounds.Center.x : axis == 1 ? value.Bounds.Center.y : value.Bounds.Center.z;
        };
        std::nth_element(_casters.begin() + first, _casters.begin() + first + half, _casters.begin() + first + count,
            [&](const auto& a, const auto& b) { return coordinate(a) < coordinate(b); });
        const UINT left = BuildNode(first, half), right = BuildNode(first + half, count - half);
        _nodes[index] = {bounds, 0, 0, left, right};
    }
    return index;
}

void SceneLocalShadows::RebuildCasters(UINT slot)
{
    _casters.clear();
    _nodes.clear();
    const auto& frame = *_resources.FrameConstants.at(slot);
    const auto* commands = _resources.IndirectCommandsCache->GetCPUData();
    const auto* instances = frame.InstanceCache->GetCPUData();
    const auto* materials = frame.MaterialCache->GetCPUData();
    for (UINT i = 0; i < _resources.IndirectCommandsCache->GetElementCount(); ++i)
    {
        const auto& command = commands[i];
        if (!command.InstanceCount) continue;
        if (command.InstanceID >= frame.InstanceCache->GetElementCount())
            throw std::out_of_range("Local shadow command references a missing instance");
        const auto& instance = instances[command.InstanceID];
        if (!instance.CastsShadows) continue;
        if (instance.MaterialIndex >= frame.MaterialCache->GetElementCount())
            throw std::out_of_range("Local shadow instance references a missing material");
        const auto& material = materials[instance.MaterialIndex];
        if (material.RenderLayer == 2 || material.ReflectionProbePreview) continue;
        _casters.push_back({BoundingBox(instance.BoundingBoxCenter, instance.BoundingBoxExtents), command});
    }
    if (!_casters.empty()) BuildNode(0, UINT(_casters.size()));
    _treeReady = true;
}

void SceneLocalShadows::Gather(UINT index, const GDX12LocalLightConstants& light,
    const std::array<GDX12LocalShadowFrustum, 6>& faces, UINT faceMask, LightWork& output) const
{
    const auto& node = _nodes[index];
    if (!LocalShadowRangeIntersects(light, node.Bounds)) return;
    for (UINT face = 0; face < LocalShadowFaceCount(light); ++face)
        if ((faceMask & (1u << face)) && !faces[face].Intersects(node.Bounds)) faceMask &= ~(1u << face);
    if (!faceMask) return;
    if (!node.Count)
    {
        Gather(node.Left, light, faces, faceMask, output);
        Gather(node.Right, light, faces, faceMask, output);
        return;
    }
    for (UINT i = node.First; i < node.First + node.Count; ++i)
    {
        const auto& caster = _casters[i];
        if (!LocalShadowRangeIntersects(light, caster.Bounds)) continue;
        for (UINT face = 0; face < LocalShadowFaceCount(light); ++face)
            if ((faceMask & (1u << face)) && faces[face].Intersects(caster.Bounds)) output.Faces[face].push_back(caster.Command);
    }
}

void SceneLocalShadows::CullJob::Execute(size_t index)
{
    const auto& light = Frame->Lights[index];
    auto& output = Frame->Lists[index];
    for (auto& face : output.Faces) face.clear();
    if (!(light.ShadowFlags & 1u) || (light.Type != 1 && light.Type != 2) || Owner->_nodes.empty() ||
        !Owner->_resources.ShadowQuality.LocalResolution) return;
    const BoundingBox influence(light.Position, XMFLOAT3(light.Range, light.Range, light.Range));
    if (!Frame->Camera->Intersects(influence)) return;
    const std::array<GDX12LocalShadowFrustum, 6> faces = {
        GDX12LocalShadowFrustum(light.ShadowViewProj[0]), GDX12LocalShadowFrustum(light.ShadowViewProj[1]),
        GDX12LocalShadowFrustum(light.ShadowViewProj[2]), GDX12LocalShadowFrustum(light.ShadowViewProj[3]),
        GDX12LocalShadowFrustum(light.ShadowViewProj[4]), GDX12LocalShadowFrustum(light.ShadowViewProj[5])};
    Owner->Gather(0, light, faces, (1u << LocalShadowFaceCount(light)) - 1, output);
}

void SceneLocalShadows::PackJob::Execute(size_t)
{
    Frame->Views.clear();
    Frame->Packed.clear();
    UINT slices = 0;
    for (UINT lightIndex = 0; lightIndex < Frame->Lights.size(); ++lightIndex)
    {
        auto& light = Frame->Lights[lightIndex];
        light.ShadowFirstSlice = UINT_MAX;
        const auto& lists = Frame->Lists[lightIndex].Faces;
        if (std::all_of(lists.begin(), lists.end(), [](const auto& list) { return list.empty(); })) continue;
        const UINT faces = LocalShadowFaceCount(light);
        if (slices + faces > D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION)
            throw std::length_error("Visible local shadow maps exceed the D3D12 array slice limit");
        light.ShadowFirstSlice = slices;
        for (UINT face = 0; face < faces; ++face)
        {
            if (Frame->Packed.size() + lists[face].size() > UINT_MAX / sizeof(GDX12IndirectDrawArgs))
                throw std::length_error("Local shadow indirect buffer exceeds the buffer size limit");
            Frame->Views.push_back({lightIndex, face, slices++, UINT(Frame->Packed.size()), UINT(lists[face].size())});
            Frame->Packed.insert(Frame->Packed.end(), lists[face].begin(), lists[face].end());
        }
    }
}

void SceneLocalShadows::Freeze(UINT slot, bool sceneChanged)
{
    if (!_common.Jobs) throw std::logic_error("Local shadow preparation requires JobSystem");
    if (sceneChanged || !_treeReady) RebuildCasters(slot);
    auto& frame = _slots.at(slot);
    frame.Lights = _resources.LocalLights;
    for (const auto& light : frame.Lights)
        if (!std::isfinite(light.Position.x) || !std::isfinite(light.Position.y) || !std::isfinite(light.Position.z) ||
            !std::isfinite(light.Range) || light.Range <= 0)
            throw std::invalid_argument("Invalid local shadow light bounds");
    frame.Lists.resize(frame.Lights.size());
    XMFLOAT4X4 viewProjection;
    XMStoreFloat4x4(&viewProjection, XMMatrixTranspose(_common.CameraView * _common.CameraProjection));
    frame.Camera.emplace(viewProjection);
    frame.Ready = frame.Cull.Schedule(*_common.Jobs, frame.Lights.size(), 1);
    frame.Ready = frame.Pack.Schedule(*_common.Jobs, 1, frame.Ready);
}

void SceneLocalShadows::Complete(UINT slot)
{
    auto& frame = _slots.at(slot);
    frame.Ready.Complete();
    const UINT lightCount = (std::max)(1u, UINT(frame.Lights.size()));
    if (!frame.LightUpload)
    {
        frame.LightUpload = std::make_unique<GDX12UploadBuffer<GDX12LocalLightConstants>>(_resources.Device, lightCount, EBufferType::Upload, false);
        frame.LightUpload->TrackChanges();
    }
    frame.LightUpload->Resize(lightCount);
    if (frame.Lights.empty()) frame.LightUpload->CopyData(0, GDX12LocalLightConstants{});
    else frame.LightUpload->CopyData(0, frame.Lights.data(), UINT(frame.Lights.size()));
    const UINT commandCount = (std::max)(1u, UINT(frame.Packed.size()));
    if (!frame.CommandUpload)
    {
        frame.CommandUpload = std::make_unique<GDX12UploadBuffer<GDX12IndirectDrawArgs>>(_resources.Device, commandCount, EBufferType::Upload, false);
        frame.Commands = std::make_unique<GDX12UploadBuffer<GDX12IndirectDrawArgs>>(_resources.Device, commandCount, EBufferType::Default, false);
    }
    frame.CommandUpload->Resize(commandCount);
    frame.Commands->Resize(commandCount);
    if (!frame.Packed.empty()) frame.CommandUpload->CopyData(0, frame.Packed.data(), UINT(frame.Packed.size()));
    PrepareAtlas(frame);
}

void SceneLocalShadows::PrepareAtlas(Slot& frame)
{
    const UINT resolution = (std::max)(1u, _resources.ShadowQuality.LocalResolution);
    const UINT slices = (std::max)(1u, UINT(frame.Views.size()));
    if (frame.Resolution == resolution && frame.Slices >= slices) return;
    GDX12TextureDesc texture;
    texture.Width = texture.Height = resolution;
    texture.Format = DXGI_FORMAT_D32_FLOAT;
    texture.SRVDesc.Format = DXGI_FORMAT_R32_FLOAT;
    texture.SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    texture.SRVDesc.Texture2DArray.MipLevels = 1;
    texture.SRVDesc.Texture2DArray.ArraySize = slices;
    texture.SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    texture.SRV_UAV_Heap = _resources.SRV_UAV_Heap.get();
    texture.SRVHeapIndex = frame.Atlas.IsInitialized() ? frame.Atlas.GetSRV()->HeapIndex : texture.SRV_UAV_Heap->GetAvailableIndex(105000);
    const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32_TYPELESS, resolution, resolution,
        UINT16(slices), 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    D3D12_CLEAR_VALUE clear{};
    clear.Format = DXGI_FORMAT_D32_FLOAT;
    clear.DepthStencil.Depth = 1;
    texture.ExternalResource = _resources.Device->CreateResource(D3D12_HEAP_TYPE_DEFAULT, desc, D3D12_RESOURCE_STATE_COMMON, &clear);
    frame.Atlas.Initialize(texture);
    frame.Dsvs.resize(slices);
    for (UINT slice = 0; slice < slices; ++slice)
    {
        auto& view = frame.Dsvs[slice];
        if (!view) view = std::make_unique<GDX12Descriptor>();
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
        dsv.Format = DXGI_FORMAT_D32_FLOAT;
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        dsv.Texture2DArray.FirstArraySlice = slice;
        dsv.Texture2DArray.ArraySize = 1;
        auto* heap = _resources.DSVHeap.get();
        view->InitAsDSV(texture.ExternalResource.Get(), &dsv, heap, view->HeapIndex == UINT_MAX ? heap->GetAvailableIndex() : view->HeapIndex);
    }
    frame.Resolution = resolution;
    frame.Slices = slices;
}

void SceneLocalShadows::Append(Graph& graph, std::vector<ID3D12Resource*>& bindings, UINT slot,
    const std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 4>& scene, GDX12GeometryBuffer::RecordingBinding geometry,
    GDX12UploadBuffer<GDX12LocalLightConstants>& lights)
{
    auto& frame = _slots.at(slot);
    auto& work = frame.Work;
    work.Commands.clear();
    const auto resource = [&](ID3D12Resource* value, const char* name) { return NativeWork::ResourceFor(graph, bindings, value, name); };
    const Id commands = resource(frame.Commands->GetResource().D3DResource.Get(), "CPU-culled local shadow commands");
    auto* target = frame.Commands->GetResource().D3DResource.Get();
    if (!frame.Packed.empty())
    {
        auto* source = frame.CommandUpload->GetResource().D3DResource.Get();
        const UINT64 bytes = UINT64(frame.Packed.size()) * sizeof(GDX12IndirectDrawArgs);
        work.Add(graph, "local shadow packed command upload",
            {{resource(source, "local shadow command upload"), Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ},
             {commands, Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST}},
            [=](auto* cmd) { cmd->CopyBufferRegion(target, 0, source, 0, bytes); });
    }
    const Id atlas = resource(frame.Atlas.GetResource()->D3DResource.Get(), "local shadow depth array");
    const auto views = frame.Views;
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> dsvs;
    for (const auto& view : frame.Dsvs) dsvs.push_back(view->CPUHandle);
    const State read = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    std::vector<Access> accesses{{0, Mode::Read, read}, {2, Mode::Read, read}, {3, Mode::Read, read},
        {21, Mode::Read, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE},
        {22, Mode::Read, D3D12_RESOURCE_STATE_INDEX_BUFFER}, {commands, Mode::Read, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT},
        {resource(lights.GetResource().D3DResource.Get(), "local shadow light data"), Mode::Read, read},
        {atlas, Mode::Write, D3D12_RESOURCE_STATE_DEPTH_WRITE}};
    auto* heap = _resources.SRV_UAV_Heap->GetHeap().Get();
    auto* root = _root.get();
    auto* pipeline = _draw.Get();
    auto* signature = _signature.Get();
    const auto lightSrv = lights.GetSRV()->GPUHandle;
    const auto textures = _resources.SRV_UAV_Heap->GetGPUHandle(Texture2D_StartIndex);
    const UINT resolution = frame.Resolution;
    work.Add(graph, "local shadow CPU lists ExecuteIndirect", std::move(accesses), [=](auto* cmd) {
        cmd->SetDescriptorHeaps(1, &heap);
        cmd->SetGraphicsRootSignature(root->GetRootSignature().Get());
        cmd->SetPipelineState(pipeline);
        const UINT inputs[] = {2, 3, 0};
        for (UINT i = 0; i < 3; ++i) cmd->SetGraphicsRootDescriptorTable(root->GetTRootParamIndex(i), scene[inputs[i]]);
        cmd->SetGraphicsRootDescriptorTable(root->GetTRootParamIndex(3), lightSrv);
        cmd->SetGraphicsRootDescriptorTable(root->GetTRootParamIndex(4), textures);
        const auto geometryRoot = root->GetGeometryRootParamIndex();
        cmd->SetGraphicsRootShaderResourceView(geometryRoot, geometry.Vertices.BufferLocation);
        cmd->SetGraphicsRootShaderResourceView(geometryRoot + 1, geometry.Vertices.BufferLocation);
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmd->IASetVertexBuffers(0, 1, &geometry.Vertices);
        cmd->IASetIndexBuffer(&geometry.Indices);
        const D3D12_VIEWPORT viewport{0, 0, float(resolution), float(resolution), 0, 1};
        const D3D12_RECT rect{0, 0, LONG(resolution), LONG(resolution)};
        cmd->RSSetViewports(1, &viewport);
        cmd->RSSetScissorRects(1, &rect);
        if (views.empty()) cmd->ClearDepthStencilView(dsvs[0], D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
        for (const auto& view : views)
        {
            const auto dsv = dsvs[view.Slice];
            cmd->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
            cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
            cmd->SetGraphicsRoot32BitConstant(0, 0, 0);
            cmd->SetGraphicsRoot32BitConstant(0, view.Light * 6 + view.Face, 1);
            if (view.Count) cmd->ExecuteIndirect(signature, view.Count, target, UINT64(view.First) * sizeof(GDX12IndirectDrawArgs), nullptr, 0);
        }
    });
}
}

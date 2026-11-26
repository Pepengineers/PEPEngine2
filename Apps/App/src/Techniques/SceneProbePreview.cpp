#include "SceneProbePreview.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include "Engine.RendererDX12/GDX12ShaderCompiler.h"
#include "Engine.RendererDX12/GDX12TextureResource.h"

namespace BistroRender
{
SceneProbePreview::SceneProbePreview(GDX12DeviceResources& resources, RenderPipelineCommonData& common)
    : _resources(resources), _common(common), _frames(resources.FrameConstants.size())
{
    GDX12RootSignatureDesc root;
    root.Constants = {3};
    root.NumSingleCBVSlots = 1;
    root.NumSingleSRVSlots = 4;
    root.StaticSamplers = GetStaticSamplers();
    _root = std::make_unique<GDX12RootSignature>(resources.Device, root);
    auto& compiler = GDX12ShaderCompiler::GetInstance();
    const auto vs = compiler.CompileShader(resources.Device, SHADERS_FOLDER "ProbePreview.hlsl", nullptr, "VS", "vs");
    const auto ps = compiler.CompileShader(resources.Device, SHADERS_FOLDER "ProbePreview.hlsl", nullptr, "PS", "ps");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};
    p.pRootSignature = _root->GetRootSignature().Get();
    p.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    p.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    p.InputLayout = {resources.InputLayouts.at("Default").data(), UINT(resources.InputLayouts.at("Default").size())};
    p.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    p.RasterizerState.FrontCounterClockwise = TRUE;
    p.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    p.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    p.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    p.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    p.NumRenderTargets = 2;
    p.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    p.RTVFormats[1] = DXGI_FORMAT_R16G16_FLOAT;
    p.SampleMask = UINT_MAX;
    p.SampleDesc.Count = 1;
    p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    ThrowIfFailed(resources.Device->GetDevice()->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&_pipeline)));
    root = {};
    root.NumSingleCBVSlots = 2;
    root.NumSingleSRVSlots = 5;
    root.StaticSamplers = GetStaticSamplers();
    root.StaticSamplers.emplace_back(7, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    _backgroundRoot = std::make_unique<GDX12RootSignature>(resources.Device, root);
    const auto fullscreen = compiler.CompileShader(resources.Device, SHADERS_FOLDER "FullScreenVS.hlsl", nullptr, "VS", "vs");
    const auto background = compiler.CompileShader(resources.Device, SHADERS_FOLDER "ProbePreviewBackground.hlsl", nullptr, "PS", "ps");
    p.pRootSignature = _backgroundRoot->GetRootSignature().Get();
    p.InputLayout = {};
    p.VS = {fullscreen->GetBufferPointer(), fullscreen->GetBufferSize()};
    p.PS = {background->GetBufferPointer(), background->GetBufferSize()};
    p.DepthStencilState.DepthEnable = FALSE;
    p.DSVFormat = DXGI_FORMAT_UNKNOWN;
    p.NumRenderTargets = 1;
    p.RTVFormats[1] = DXGI_FORMAT_UNKNOWN;
    ThrowIfFailed(resources.Device->GetDevice()->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&_backgroundPipeline)));
}
void SceneProbePreview::Append(Graph& graph, std::vector<ID3D12Resource*>& bindings, SceneGeometry& geometry,
    SceneReflections& reflections, UINT slot, GDX12Texture* hdr, GDX12Texture* inputDepth,
    GDX12Texture* inputMotion, SceneEnvironment& environment)
{
    auto& work = _work;
    work.Commands.clear();
    _depth = inputDepth;
    _motion = inputMotion;
    auto& constants = *_resources.FrameConstants.at(slot);
    struct Draw { GDX12IndirectDrawArgs Args; UINT Cube; UINT Reset; };
    std::vector<Draw> draws;
    std::unordered_set<UINT64> current;
    const auto* commands = _resources.IndirectCommandsCache->GetCPUData();
    const auto* instances = constants.InstanceCache->GetCPUData();
    const auto* materials = constants.MaterialCache->GetCPUData();
    const auto revision = _resources.IndirectCommandsCache->GetRevision();
    auto source = _resources.IndirectCommandsCache->GetResource().D3DResource;
    if (_commandRevision != revision || _commandSource.Get() != source.Get())
    {
        _previewCommands.clear();
        for (UINT i = 0; i < _resources.IndirectCommandsCache->GetElementCount(); ++i)
        {
            const auto& args = commands[i];
            if (!args.InstanceCount) continue;
            if (args.InstanceID >= constants.InstanceCache->GetElementCount())
                throw std::out_of_range("Probe preview instance exceeds the frozen cache");
            const auto& instance = instances[args.InstanceID];
            if (instance.MaterialIndex >= constants.MaterialCache->GetElementCount())
                throw std::out_of_range("Probe preview material exceeds the frozen cache");
            if (materials[instance.MaterialIndex].ReflectionProbePreview) _previewCommands.push_back(args);
        }
        _commandRevision = revision;
        _commandSource = source;
    }
    for (const auto& args : _previewCommands)
    {
        const auto& instance = instances[args.InstanceID];
        const UINT preview = materials[instance.MaterialIndex].ReflectionProbePreview;
        if (!preview) continue;
        const UINT64 key = (UINT64(instance.TransformIndex) << 32) | preview;
        current.insert(key);
        draws.push_back({args, preview - 1, UINT(_common.ResetHistory || _previous.find(key) == _previous.end())});
    }
    _previous = std::move(current);
    auto& frame = _frames.at(slot);
    const UINT width = inputDepth->GetWidth(), height = inputDepth->GetHeight();
    // Append runs only after this frame slot retires; no descriptor is replaced while in flight.
    if (!draws.empty() && (!frame.Depth || frame.Depth->GetWidth() != width || frame.Depth->GetHeight() != height))
    {
        const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32_TYPELESS, width, height, 1, 1, 1, 0,
            D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
        D3D12_CLEAR_VALUE clear{};
        clear.Format = DXGI_FORMAT_D32_FLOAT;
        frame.DepthResource = _resources.Device->CreateResource(D3D12_HEAP_TYPE_DEFAULT, desc,
            D3D12_RESOURCE_STATE_COMMON, &clear);
        if (!frame.Depth) frame.Depth = std::make_unique<GDX12Texture>();
        GDX12TextureDesc view;
        view.ExternalResource = frame.DepthResource;
        view.Width = width; view.Height = height;
        view.Format = DXGI_FORMAT_R32_FLOAT;
        view.SRV_UAV_Heap = _resources.SRV_UAV_Heap.get();
        view.SRVHeapIndex = frame.Depth->IsInitialized() ? frame.Depth->GetSRV()->HeapIndex :
            view.SRV_UAV_Heap->GetAvailableIndex(SceneDescriptorStart);
        view.SRVDesc.Format = DXGI_FORMAT_R32_FLOAT;
        view.SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.SRVDesc.Texture2D.MipLevels = 1;
        frame.Depth->Initialize(view);
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
        dsv.Format = DXGI_FORMAT_D32_FLOAT;
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        frame.Dsv.InitAsDSV(frame.DepthResource.Get(), &dsv, _resources.DSVHeap.get(),
            frame.Dsv.HeapIndex == UINT_MAX ? _resources.DSVHeap->GetAvailableIndex() : frame.Dsv.HeapIndex);
        dsv.Flags = D3D12_DSV_FLAG_READ_ONLY_DEPTH;
        frame.ReadDsv.InitAsDSV(frame.DepthResource.Get(), &dsv, _resources.DSVHeap.get(),
            frame.ReadDsv.HeapIndex == UINT_MAX ? _resources.DSVHeap->GetAvailableIndex() : frame.ReadDsv.HeapIndex);
        if (!frame.Motion)
        {
            frame.Motion = std::make_unique<GDX12Texture>();
            GDX12RenderHelpers::InitializeRenderTarget(*frame.Motion, _resources, width, height, DXGI_FORMAT_R16G16_FLOAT);
        }
        else frame.Motion->Resize(width, height);
    }
    if (!frame.HDR)
    {
        frame.HDR = std::make_unique<GDX12Texture>();
        GDX12RenderHelpers::InitializeRenderTarget(*frame.HDR, _resources, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    }
    else if (frame.HDR->GetWidth() != width || frame.HDR->GetHeight() != height) frame.HDR->Resize(width, height);
    _hdr = frame.HDR.get();
    const auto resource = [&](ID3D12Resource* value) {
        return NativeWork::ResourceFor(graph, bindings, value, "probe preview resource");
    };
    const auto texture = [&](GDX12Texture* value, Mode mode, State state) {
        return Access{resource(value->GetResource()->D3DResource.Get()), mode, state};
    };
    if (draws.empty())
        _readDepth = geometry.ReadOnlyDepthDSV(slot);
    else
    {
        _readDepth = frame.ReadDsv.CPUHandle;
        _depth = frame.Depth.get();
        _motion = frame.Motion.get();
        auto* sourceDepth = inputDepth->GetResource()->D3DResource.Get();
        auto* sourceMotion = inputMotion->GetResource()->D3DResource.Get();
        auto* depth = frame.DepthResource.Get();
        auto* motion = frame.Motion->GetResource()->D3DResource.Get();
        work.Add(graph, "probe preview preserve opaque depth/motion",
            {texture(inputDepth, Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE),
             texture(inputMotion, Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE),
             texture(frame.Depth.get(), Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST),
             texture(frame.Motion.get(), Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST)},
            [=](auto* cmd) { cmd->CopyResource(depth, sourceDepth); cmd->CopyResource(motion, sourceMotion); });
    }
    auto& scene = geometry._slots.at(slot);
    const State read = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    std::vector<Access> backgroundReads;
    environment.AddAerialReads(graph, bindings, backgroundReads);
    backgroundReads.insert(backgroundReads.end(), {
        texture(hdr, Mode::Read, read), texture(inputDepth, Mode::Read, read),
        texture(frame.HDR.get(), Mode::Write, D3D12_RESOURCE_STATE_RENDER_TARGET),
        {scene.FrameIds[1], Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ},
        {resource(constants.SkyCB->GetResource().D3DResource.Get()), Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ}});
    auto* backgroundRoot = _backgroundRoot.get();
    auto* backgroundPipeline = _backgroundPipeline.Get();
    const std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 5> backgroundSRVs{hdr->GetSRV()->GPUHandle,
        inputDepth->GetSRV()->GPUHandle, _resources.SkyTransmittanceSRV.GPUHandle,
        _resources.SkyAerialRadianceSRV.GPUHandle, _resources.SkyAerialTransmittanceSRV.GPUHandle};
    const auto skyCB = constants.SkyCB->GetElementAddress(0);
    const auto cameraCB = scene.Camera;
    const auto backgroundRTV = frame.HDR->GetRTV()->CPUHandle;
    const auto backgroundViewport = hdr->GetViewport();
    const auto backgroundRect = hdr->GetScissorRect();
    auto* backgroundHeap = _resources.SRV_UAV_Heap->GetHeap().Get();
    work.Add(graph, "probe preview opaque aerial background", std::move(backgroundReads), [=](auto* cmd) {
        cmd->SetDescriptorHeaps(1, &backgroundHeap);
        cmd->SetGraphicsRootSignature(backgroundRoot->GetRootSignature().Get());
        cmd->SetPipelineState(backgroundPipeline);
        cmd->SetGraphicsRootConstantBufferView(backgroundRoot->GetBRootParamIndex(0), cameraCB);
        cmd->SetGraphicsRootConstantBufferView(backgroundRoot->GetBRootParamIndex(1), skyCB);
        for (UINT i = 0; i < backgroundSRVs.size(); ++i)
            cmd->SetGraphicsRootDescriptorTable(backgroundRoot->GetTRootParamIndex(i), backgroundSRVs[i]);
        cmd->RSSetViewports(1, &backgroundViewport);
        cmd->RSSetScissorRects(1, &backgroundRect);
        cmd->OMSetRenderTargets(1, &backgroundRTV, FALSE, nullptr);
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmd->DrawInstanced(3, 1, 0, 0);
    });
    if (draws.empty()) return;
    std::vector<Access> accesses{
        texture(frame.HDR.get(), Mode::ReadWrite, D3D12_RESOURCE_STATE_RENDER_TARGET),
        texture(frame.Depth.get(), Mode::ReadWrite, D3D12_RESOURCE_STATE_DEPTH_WRITE),
        texture(frame.Motion.get(), Mode::ReadWrite, D3D12_RESOURCE_STATE_RENDER_TARGET),
        texture(reflections.Published(), Mode::Read, read),
        {scene.FrameIds[1], Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ},
        {resource(constants.ReflectionProbes->GetResource().D3DResource.Get()), Mode::Read, read},
        {resource(scene.Buffers[0].Data.Get()), Mode::Read, read},
        {resource(scene.Buffers[3].Data.Get()), Mode::Read, read},
        {resource(scene.Geometry.VertexResource), Mode::Read, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER},
        {resource(scene.Geometry.IndexResource), Mode::Read, D3D12_RESOURCE_STATE_INDEX_BUFFER}};
    const auto geometryBinding = scene.Geometry;
    const auto camera = scene.Camera;
    const std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 4> srvs{scene.Buffers[3].Srv.GPUHandle,
        scene.Buffers[0].Srv.GPUHandle, constants.ReflectionProbes->GetSRV()->GPUHandle, reflections.Radiance()};
    const std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 2> rtvs{frame.HDR->GetRTV()->CPUHandle, frame.Motion->GetRTV()->CPUHandle};
    const auto dsv = frame.Dsv.CPUHandle;
    const auto viewport = hdr->GetViewport();
    const auto rect = hdr->GetScissorRect();
    auto* root = _root.get();
    auto* pipeline = _pipeline.Get();
    auto* heap = _resources.SRV_UAV_Heap->GetHeap().Get();
    work.Add(graph, "probe preview unlit HDR spheres", std::move(accesses), [=](auto* cmd) {
        cmd->SetDescriptorHeaps(1, &heap);
        cmd->SetGraphicsRootSignature(root->GetRootSignature().Get());
        cmd->SetPipelineState(pipeline);
        cmd->SetGraphicsRootConstantBufferView(root->GetBRootParamIndex(1), camera);
        for (UINT i = 0; i < srvs.size(); ++i)
            cmd->SetGraphicsRootDescriptorTable(root->GetTRootParamIndex(i), srvs[i]);
        cmd->RSSetViewports(1, &viewport);
        cmd->RSSetScissorRects(1, &rect);
        cmd->OMSetRenderTargets(UINT(rtvs.size()), rtvs.data(), FALSE, &dsv);
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmd->IASetVertexBuffers(0, 1, &geometryBinding.Vertices);
        cmd->IASetIndexBuffer(&geometryBinding.Indices);
        for (const auto& draw : draws)
        {
            const UINT values[]{draw.Args.InstanceID, draw.Cube, draw.Reset};
            cmd->SetGraphicsRoot32BitConstants(root->GetBRootParamIndex(0), 3, values, 0);
            cmd->DrawIndexedInstanced(draw.Args.IndexCountPerInstance, 1, draw.Args.StartIndexLocation,
                draw.Args.BaseVertexLocation, 0);
        }
    });
}
}

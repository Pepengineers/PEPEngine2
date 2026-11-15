#include "SceneTransparency.h"
#include "Common/GameTimer.h"

namespace Meta
{
SceneTransparency::SceneTransparency(GDX12DeviceResources& resources, RenderPipelineCommonData& common)
    : _resources(resources), _common(common), _frames(resources.FrameConstants.size()), _particles(resources, common),
      _water(resources, common)
{
    for (auto& frame : _frames) for (auto* phase : {&frame.All, &frame.Far, &frame.Near})
    {
        phase->Draw.SetFlag(RENDER_PASS_FLAG_USE_DOWNSCALED_RESOLUTION, true);
        phase->Draw.SetUnbatchedDraws(true);
        phase->Draw.SetStaticIBLOnly(true);
        phase->Compose.SetFlag(RENDER_PASS_FLAG_USE_DOWNSCALED_RESOLUTION, true);
        phase->Compose.SetApplyOpaqueAerialPerspective(false);
    }
}
SceneTransparency::~SceneTransparency()
{
    for (auto& frame : _frames) for (auto* phase : {&frame.All, &frame.Far, &frame.Near})
    {
        phase->Compose.ClearDenendencies();
        phase->Draw.ClearDenendencies();
    }
}
void SceneTransparency::PrepareWater(Graph& graph, std::vector<ID3D12Resource*>& bindings,
    SceneGeometry& geometry, UINT slot)
{
    _water.Prepare(_common.GameTimer ? _common.GameTimer->DeltaTime() : 0);
    _water.AppendSurface(graph, bindings, geometry.Depth(slot), geometry.GBuffer(slot, 1));
}
GDX12Texture* SceneTransparency::Append(Graph& graph, std::vector<ID3D12Resource*>& bindings,
    SceneGeometry& geometry, UINT slot, GDX12Texture* hdr, GDX12Texture* sun,
    GDX12Texture* local, SceneEnvironment& environment)
{
    _work.Commands.clear();
    auto& frame = _frames.at(slot);
    if (_water.Active())
    {
        auto* farHDR = AppendPhase(graph, bindings, geometry, slot, frame.Far, hdr, sun, local, environment, false);
        auto* water = _water.Resolve(graph, bindings, farHDR, environment);
        const auto resolved = Id(graph.Operations.size() - 1);
        return AppendPhase(graph, bindings, geometry, slot, frame.Near, water, sun, local, environment, true, resolved);
    }
    return AppendPhase(graph, bindings, geometry, slot, frame.All, hdr, sun, local, environment, true);
}
GDX12Texture* SceneTransparency::AppendPhase(Graph& graph, std::vector<ID3D12Resource*>& bindings,
    SceneGeometry& geometry, UINT slot, Phase& phase, GDX12Texture* hdr, GDX12Texture* sun,
    GDX12Texture* local, SceneEnvironment& environment, bool particles, Id predecessor)
{
    auto& scene = geometry._slots.at(slot);
    auto& constants = *_resources.FrameConstants.at(slot);
    auto& draw = phase.Draw;
    auto& compose = phase.Compose;
    const bool partitioned = draw._partition != GDX12GeometryDepthPartition::All;
    auto* rasterDepth = _water.Depth();
    draw.Setup(&_resources, &_common);
    std::vector<IRenderPassLink*> inputs{scene.DepthView.get(), sun, environment.Output(0), environment.Output(1),
        environment.Output(2), local};
    if (partitioned) inputs.push_back(rasterDepth);
    draw.SetInputs(std::move(inputs));
    if (!phase.Initialized) draw.Initialize();
    else if (draw.OUT_Accumulation->GetWidth() != hdr->GetWidth() ||
             draw.OUT_Accumulation->GetHeight() != hdr->GetHeight()) draw.Resize();
    auto* accum = draw.OUT_Accumulation.get();
    auto* reveal = draw.OUT_Revealage.get();
    compose.Setup(&_resources, &_common);
    compose.SetInputs({hdr, accum, reveal});
    if (!phase.Initialized) compose.Initialize();
    else if (compose.OUT_Result->GetWidth() != hdr->GetWidth() ||
             compose.OUT_Result->GetHeight() != hdr->GetHeight()) compose.Resize();
    phase.Initialized = true;
    auto* output = compose.OUT_Result.get();
    if (particles)
        _particles.Prepare(graph, bindings, scene.DepthView.get(), rasterDepth, accum, reveal, geometry.InteractionField());
    const auto resource = [&](ID3D12Resource* value) {
        return NativeWork::ResourceFor(graph, bindings, value, "mesh transparency resource");
    };
    const auto texture = [&](GDX12Texture* value, Mode mode, State state) {
        return Access{resource(value->GetResource()->D3DResource.Get()), mode, state};
    };
    const State read = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    const State depthRead = D3D12_RESOURCE_STATE_DEPTH_READ | read;
    std::vector<Access> accesses{texture(scene.DepthView.get(), Mode::Read, depthRead),
        texture(sun, Mode::Read, read), texture(local, Mode::Read, read),
        texture(environment.Output(0)->GetTexture(), Mode::Read, read),
        texture(environment.Output(1)->GetTexture(), Mode::Read, read),
        texture(environment.Output(2)->GetTexture(), Mode::Read, read),
        {scene.FrameIds[0], Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ},
        {scene.FrameIds[1], Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ},
        {resource(constants.ShadowCB->GetResource().D3DResource.Get()), Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ},
        {resource(constants.ShadowLights->GetResource().D3DResource.Get()), Mode::Read, read},
        texture(accum, Mode::Write, D3D12_RESOURCE_STATE_RENDER_TARGET),
        texture(reveal, Mode::Write, D3D12_RESOURCE_STATE_RENDER_TARGET)};
    if (partitioned) accesses.push_back(texture(rasterDepth, Mode::Read, read));
    for (UINT i : {0u, 2u, 3u})
        accesses.push_back({resource(scene.Buffers[i].Data.Get()), Mode::Read, read});
    for (UINT i : {6u, 7u})
        accesses.push_back({resource(scene.Buffers[i].Data.Get()), Mode::Read, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT});
    accesses.push_back({resource(scene.Geometry.VertexResource), Mode::Read,
        D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE});
    accesses.push_back({resource(scene.Geometry.IndexResource), Mode::Read, D3D12_RESOURCE_STATE_INDEX_BUFFER});
    auto* root = draw._transparencyRS.get();
    auto* pipeline = draw._transparencyPSO.Get();
    auto* signature = draw._transparencyCS.Get();
    auto* heap = _resources.SRV_UAV_Heap->GetHeap().Get();
    const auto main = scene.Main, camera = scene.Camera, shadow = constants.ShadowCB->GetElementAddress(0);
    const std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 9> srvs{scene.Buffers[2].Srv.GPUHandle,
        scene.Buffers[3].Srv.GPUHandle, scene.Buffers[0].Srv.GPUHandle, sun->GetSRV()->GPUHandle,
        environment.Output(0)->GetTexture()->GetSRV()->GPUHandle,
        environment.Output(1)->GetTexture()->GetSRV()->GPUHandle,
        environment.Output(2)->GetTexture()->GetSRV()->GPUHandle,
        constants.ShadowLights->GetSRV()->GPUHandle, local->GetSRV()->GPUHandle};
    const auto textures = _resources.SRV_UAV_Heap->GetGPUHandle(Texture2D_StartIndex);
    const auto geom = scene.Geometry;
    const auto depth = scene.ReadDepth.CPUHandle;
    const D3D12_VIEWPORT viewport = accum->GetViewport();
    const D3D12_RECT rect = accum->GetScissorRect();
    const std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 2> targets{accum->GetRTV()->CPUHandle, reveal->GetRTV()->CPUHandle};
    const UINT draws = scene.Draws;
    auto* args = scene.Buffers[6].Data.Get();
    auto* count = scene.Buffers[7].Data.Get();
    const auto separator = rasterDepth->GetSRV()->GPUHandle;
    const auto opaqueDepth = scene.DepthView->GetSRV()->GPUHandle;
    const char* drawName = !partitioned ? "mesh transparency All clear/draw" : particles ?
        "mesh transparency Near clear/draw" : "mesh transparency Far clear/draw";
    _work.Add(graph, drawName, std::move(accesses), [=](auto* cmd) {
        const float zero[4]{}, one[4]{1, 1, 1, 1};
        cmd->ClearRenderTargetView(targets[0], zero, 0, nullptr);
        cmd->ClearRenderTargetView(targets[1], one, 0, nullptr);
        cmd->SetDescriptorHeaps(1, &heap);
        cmd->SetGraphicsRootSignature(root->GetRootSignature().Get());
        cmd->SetPipelineState(pipeline);
        cmd->SetGraphicsRootConstantBufferView(root->GetBRootParamIndex(1), main);
        cmd->SetGraphicsRootConstantBufferView(root->GetBRootParamIndex(2), camera);
        cmd->SetGraphicsRootConstantBufferView(root->GetBRootParamIndex(3), shadow);
        for (UINT i = 0; i < srvs.size(); ++i)
            cmd->SetGraphicsRootDescriptorTable(root->GetTRootParamIndex(i), srvs[i]);
        if (partitioned)
        {
            cmd->SetGraphicsRootDescriptorTable(root->GetTRootParamIndex(14), separator);
            cmd->SetGraphicsRootDescriptorTable(root->GetTRootParamIndex(15), opaqueDepth);
        }
        cmd->SetGraphicsRootDescriptorTable(root->GetTRootParamIndex(16), textures);
        cmd->SetGraphicsRootShaderResourceView(root->GetGeometryRootParamIndex(), geom.Vertices.BufferLocation);
        cmd->RSSetViewports(1, &viewport);
        cmd->RSSetScissorRects(1, &rect);
        cmd->OMSetRenderTargets(UINT(targets.size()), targets.data(), FALSE, &depth);
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmd->IASetVertexBuffers(0, 1, &geom.Vertices);
        cmd->IASetIndexBuffer(&geom.Indices);
        if (draws) cmd->ExecuteIndirect(signature, draws, args, 0, count, 0);
    }, predecessor == Invalid ? std::vector<Id>{} : std::vector<Id>{predecessor});
    if (particles) _particles.AppendWeighted(graph, bindings);
    auto* composeRoot = compose._compositionRS.get();
    auto* composePipeline = compose._compositionPSO.Get();
    const std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 3> images{
        hdr->GetSRV()->GPUHandle, accum->GetSRV()->GPUHandle, reveal->GetSRV()->GPUHandle};
    const auto target = output->GetRTV()->CPUHandle;
    const char* resolveName = !partitioned ? "mesh WBOIT resolve" : particles ? "mesh Near WBOIT resolve" : "mesh Far WBOIT resolve";
    _work.Add(graph, resolveName, {texture(hdr, Mode::Read, read), texture(accum, Mode::Read, read),
        texture(reveal, Mode::Read, read), texture(output, Mode::Write, D3D12_RESOURCE_STATE_RENDER_TARGET)},
        [=](auto* cmd) {
            cmd->SetDescriptorHeaps(1, &heap);
            cmd->SetGraphicsRootSignature(composeRoot->GetRootSignature().Get());
            cmd->SetPipelineState(composePipeline);
            for (UINT i = 0; i < images.size(); ++i)
                cmd->SetGraphicsRootDescriptorTable(composeRoot->GetTRootParamIndex(i), images[i]);
            cmd->RSSetViewports(1, &viewport);
            cmd->RSSetScissorRects(1, &rect);
            cmd->OMSetRenderTargets(1, &target, FALSE, nullptr);
            cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            cmd->DrawInstanced(3, 1, 0, 0);
        });
    if (particles) _particles.AppendComposite(graph, bindings, output);
    return output;
}
}

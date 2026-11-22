#include "SceneTransparency.h"
#include "WboitMeshDraw.h"

namespace BistroRender
{
GDX12Texture* SceneTransparency::Append(Graph& graph, std::vector<ID3D12Resource*>& bindings, SceneGeometry& geometry,
                                        UINT slot, GDX12Texture* hdr, SceneEnvironment& environment)
{
	_work.Commands.clear();
	auto& frame = _frames.at(slot);
	if (_remote) AppendRemote(geometry, slot, frame, hdr);
	_water.AppendDraw(graph, bindings, hdr, geometry._slots.at(slot).Dsv.CPUHandle);
	if (_opaqueContinuation)
	{
		hdr = _opaqueContinuation(graph, bindings, hdr, _water.Depth(), _water.Motion());
	}
	if (_remote) return ApplyRemote(graph, bindings, geometry, slot, frame, hdr, environment);
	return AppendPhase(graph, bindings, geometry, slot, frame, hdr, environment);
}
GDX12Texture* SceneTransparency::AppendPhase(Graph& graph, std::vector<ID3D12Resource*>& bindings,
                                             SceneGeometry& geometry, UINT slot, Frame& phase, GDX12Texture* hdr,
                                             SceneEnvironment& environment)
{
	auto& scene = geometry._slots.at(slot);
	auto& constants = *_resources.FrameConstants.at(slot);
	auto& draw = phase.Draw;
	auto& compose = phase.Compose;
	auto* rasterDepth = geometry.Depth(slot);
	compose.SetApplyOpaqueAerialPerspective(!_opaqueContinuation);
	draw.Setup(&_resources, &_common);
	if (!phase.Initialized)
	{
		draw.InitializeRaster(rasterDepth);
	}
	else if (draw.OUT_Accumulation->GetWidth() != hdr->GetWidth() ||
	         draw.OUT_Accumulation->GetHeight() != hdr->GetHeight())
	{
		draw.Resize();
	}
	auto* accum = draw.OUT_Accumulation.get();
	auto* reveal = draw.OUT_Revealage.get();
	compose.Setup(&_resources, &_common);
	compose.SetInputs({hdr, accum, reveal, rasterDepth});
	if (!phase.Initialized)
	{
		compose.Initialize();
	}
	else if (compose.OUT_Result->GetWidth() != hdr->GetWidth() || compose.OUT_Result->GetHeight() != hdr->GetHeight())
	{
		compose.Resize();
	}
	phase.Initialized = true;
	auto* output = compose.OUT_Result.get();
	_particles.Prepare(graph, bindings, rasterDepth, accum, reveal);
	const auto resource = [&](ID3D12Resource* value)
	{ return NativeWork::ResourceFor(graph, bindings, value, "mesh transparency resource"); };
	const auto texture = [&](GDX12Texture* value, Mode mode, State state)
	{ return Access{resource(value->GetResource()->D3DResource.Get()), mode, state}; };
	const State read = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
	const State depthRead = D3D12_RESOURCE_STATE_DEPTH_READ | read;
	std::vector<Access> accesses{texture(rasterDepth, Mode::Read, depthRead),
	     {scene.FrameIds[0], Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ},
	     {scene.FrameIds[1], Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ},
	     texture(accum, Mode::Write, D3D12_RESOURCE_STATE_RENDER_TARGET),
	     texture(reveal, Mode::Write, D3D12_RESOURCE_STATE_RENDER_TARGET)};
	for (UINT i : {0u, 2u, 3u})
	{
		accesses.push_back({resource(scene.Buffers[i].Data.Get()), Mode::Read, read});
	}
	for (UINT i : {6u, 7u})
	{
		accesses.push_back({resource(scene.Buffers[i].Data.Get()), Mode::Read, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT});
	}
	accesses.push_back(
	    {resource(scene.Geometry.VertexResource), Mode::Read,
	     D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE});
	accesses.push_back({resource(scene.Geometry.IndexResource), Mode::Read, D3D12_RESOURCE_STATE_INDEX_BUFFER});
	auto* root = draw._transparencyRS.get();
	auto* pipeline = draw._transparencyPSO.Get();
	auto* signature = draw._transparencyCS.Get();
	auto* heap = _resources.SRV_UAV_Heap->GetHeap().Get();
	const auto main = scene.Main, camera = scene.Camera, sky = constants.SkyCB->GetElementAddress(0);
	const std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 3> srvs{scene.Buffers[2].Srv.GPUHandle,
	                                                      scene.Buffers[3].Srv.GPUHandle,
	                                                      scene.Buffers[0].Srv.GPUHandle};
	const std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 3> atmosphere{_resources.SkyTransmittanceSRV.GPUHandle,
	                                                            _resources.SkyAerialRadianceSRV.GPUHandle,
	                                                            _resources.SkyAerialTransmittanceSRV.GPUHandle};
	const auto textures = _resources.SRV_UAV_Heap->GetGPUHandle(Texture2D_StartIndex);
	const auto geom = scene.Geometry;
	const auto depth = scene.ReadDepth.CPUHandle;
	const D3D12_VIEWPORT viewport = accum->GetViewport();
	const D3D12_RECT rect = accum->GetScissorRect();
	const std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 2> targets{accum->GetRTV()->CPUHandle, reveal->GetRTV()->CPUHandle};
	const UINT draws = scene.Draws;
	auto* args = scene.Buffers[6].Data.Get();
	auto* count = scene.Buffers[7].Data.Get();
	WboitMeshDraw mesh{root, pipeline, signature, heap, main, camera, srvs, textures, geom,
	              depth, viewport, rect, targets, draws, args, count};
	_work.Add(graph, "mesh transparency All clear/draw", std::move(accesses), mesh);
	_particles.AppendWeighted(graph, bindings);
	auto* composeRoot = compose._compositionRS.get();
	auto* composePipeline = compose._compositionPSO.Get();
	const std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 7> images{hdr->GetSRV()->GPUHandle,
	                                                        accum->GetSRV()->GPUHandle,
	                                                        reveal->GetSRV()->GPUHandle,
	                                                        rasterDepth->GetSRV()->GPUHandle,
	                                                        atmosphere[0],
	                                                        atmosphere[1],
	                                                        atmosphere[2]};
	const auto target = output->GetRTV()->CPUHandle;
	const char* resolveName = "mesh WBOIT resolve";
	std::vector<Access> composition;
	if (!_opaqueContinuation) environment.AddAerialReads(graph, bindings, composition);
	composition.insert(composition.end(), {texture(hdr, Mode::Read, read),
	                                       texture(accum, Mode::Read, read),
	                                       texture(reveal, Mode::Read, read),
	                                       texture(rasterDepth, Mode::Read, read),
	                                       {scene.FrameIds[1], Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ},
	                                       {resource(constants.SkyCB->GetResource().D3DResource.Get()), Mode::Read,
	                                        D3D12_RESOURCE_STATE_GENERIC_READ},
	                                       texture(output, Mode::Write, D3D12_RESOURCE_STATE_RENDER_TARGET)});
	_work.Add(graph, resolveName, std::move(composition),
	          [=](auto* cmd)
	          {
		          cmd->SetDescriptorHeaps(1, &heap);
		          cmd->SetGraphicsRootSignature(composeRoot->GetRootSignature().Get());
		          cmd->SetPipelineState(composePipeline);
		          cmd->SetGraphicsRootConstantBufferView(composeRoot->GetBRootParamIndex(0), camera);
		          cmd->SetGraphicsRootConstantBufferView(composeRoot->GetBRootParamIndex(1), sky);
		          for (UINT i = 0; i < images.size(); ++i)
		          {
			          cmd->SetGraphicsRootDescriptorTable(composeRoot->GetTRootParamIndex(i), images[i]);
		          }
		          cmd->RSSetViewports(1, &viewport);
		          cmd->RSSetScissorRects(1, &rect);
		          cmd->OMSetRenderTargets(1, &target, FALSE, nullptr);
		          cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		          cmd->DrawInstanced(3, 1, 0, 0);
	          });
	_particles.AppendComposite(graph, bindings, output);
	return output;
}
} // namespace BistroRender

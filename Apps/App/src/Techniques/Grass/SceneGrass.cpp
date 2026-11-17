#include "SceneGrass.h"
#include "Techniques/NativeWork.h"
#include "Engine.Core/BenchmarkEngine.h"
#include "App.Base/Modules/RenderModule.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include "Engine.RendererDX12/GDX12TextureResource.h"
#include <algorithm>

namespace BistroRender
{
SceneGrass::SceneGrass(RenderPipelineContext& c)
    : _resources(c.Primary), _scene(BenchmarkEngine::GetLocator().GetModule<SceneManagerModule>(), c.Runtime)
{
	_gpu = std::make_unique<GDX12GrassRenderer>(&_resources, SceneDescriptorStart);
	_frames.resize(_resources.FrameConstants.size());
	GDX12RootSignatureDesc geometry;
	geometry.NumSingleCBVSlots = 1;
	geometry.NumSingleSRVSlots = 6;
	geometry.NumSingleUAVSlots = 3;
	geometry.VolatileSRVData = true;
	_geometryRoot = std::make_unique<GDX12RootSignature>(_resources.Device, geometry);
	D3D12_FEATURE_DATA_D3D12_OPTIONS1 options{};
	const bool waves = SUCCEEDED(_resources.Device->GetDevice()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1,
	                                                                                 &options, sizeof(options))) &&
	                   options.WaveOps;
	const D3D_SHADER_MACRO defines[] = {
	    {"GRASS_BUILD_ONCE", "1"}, {"GRASS_WAVE_OPS", waves ? "1" : "0"}, {nullptr, nullptr}};
	_buildGeometry = GDX12RenderHelpers::CreateComputePipeline(
	    _resources.Device, _geometryRoot.get(), APP_SHADERS_FOLDER "GrassExpand.hlsl", "BuildCS", defines);
	_cullGeometry = GDX12RenderHelpers::CreateComputePipeline(_resources.Device, _geometryRoot.get(),
	                                                          APP_SHADERS_FOLDER "GrassExpand.hlsl", "CullCS", defines);
	_clearGeometry = GDX12RenderHelpers::CreateComputePipeline(
	    _resources.Device, _geometryRoot.get(), APP_SHADERS_FOLDER "GrassExpand.hlsl", "ClearCS", defines);
	GDX12RootSignatureDesc hiz;
	hiz.Constants = {6};
	hiz.NumSingleSRVSlots = 1;
	hiz.NumSingleUAVSlots = 1;
	_hiZRoot = std::make_unique<GDX12RootSignature>(_resources.Device, hiz);
	_buildHiZ = GDX12RenderHelpers::CreateComputePipeline(_resources.Device, _hiZRoot.get(),
	                                                      APP_SHADERS_FOLDER "GrassHiZ.hlsl", "CS");
}
void SceneGrass::Prepare(RenderPipelineContext& c, float dt)
{
	_scene.CollectScene(dt);
	_gpu->BeginFrame(c.Common, _scene._grass, _scene._wind, _scene._controls, _scene._sources, _scene._deltaSeconds);
	_gpu->PublishPrimitives(_resources.Primitives, &_scene);
}
std::vector<GDX12RenderPrimitive> SceneGrass::Primitives() const
{
	std::vector<GDX12RenderPrimitive> result;
	for (auto id : _gpu->_drawOrder)
	{
		if (const auto* primitive = _resources.Primitives.Get(_gpu->_grass.at(id).Primitive))
		{
			result.push_back(*primitive);
		}
	}
	return result;
}
void SceneGrass::Append(Graph& g, UINT slot, std::vector<ID3D12Resource*>& all, GDX12Descriptor& materials,
                        const std::vector<GeometryTarget>& targets, bool viewsOnly,
                        const std::function<void()>& afterInteraction, const std::function<void()>& afterDepth)
{
	auto& gpu = *_gpu;
	auto& frame = gpu.CurrentFrame();
	auto& recorded = _frames.at(slot);
	if (!viewsOnly)
	{
		recorded.Commands.clear();
	}

	const auto resource = [&](ID3D12Resource* native)
	{ return NativeWork::ResourceFor(g, all, native, "grass resource"); };
	const auto access = [&](auto* buffer, Mode mode, State state)
	{ return Access{resource(buffer->GetResource().D3DResource.Get()), mode, state}; };
	const auto add =
	    [&](std::string name, std::vector<Access> accesses, std::function<void(ID3D12GraphicsCommandList*)> command)
	{
		std::vector<Access> unique;
		for (auto item : accesses)
		{
			const auto found =
			    std::find_if(unique.begin(), unique.end(), [&](const auto& a) { return a.Resource == item.Resource; });
			if (found == unique.end())
			{
				unique.push_back(item);
			}
			else if (found->Required != item.Required || found->Usage != item.Usage)
			{
				throw std::logic_error("Conflicting frozen grass access");
			}
		}
		const Id id = Id(g.Operations.size());
		g.Operations.push_back({std::move(name), std::move(unique)});
		recorded.Commands.emplace_back(id, std::move(command));
	};
	ID3D12DescriptorHeap* heap = _resources.SRV_UAV_Heap->GetHeap().Get();
	if (!viewsOnly)
	{
		for (const auto& [roots, upload] : frame.RootUploads)
		{
			const auto dst = roots->GetResource().D3DResource.Get(), src = upload->GetResource().D3DResource.Get();
			const auto bytes = UINT64(roots->GetElementCount()) * sizeof(XMFLOAT3);
			add("grass root upload",
			    {access(roots, Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST),
			     access(upload, Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ)},
			    [=](auto* cmd) { cmd->CopyBufferRegion(dst, 0, src, 0, bytes); });
		}
		{
			const auto root = gpu._interactionRoot->GetRootSignature();
			const auto pipeline = gpu._interactionPSO;
			const auto cb = frame.InteractionConstants->GetElementAddress(0);
			const auto sources = frame.Sources->GetSRV()->GPUHandle;
			const std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 3> uavs = {gpu._trail->GetUAV()->GPUHandle,
			                                                         gpu._previousTrail->GetUAV()->GPUHandle,
			                                                         gpu._presence->GetUAV()->GPUHandle};
			const UINT b = gpu._interactionRoot->GetBRootParamIndex(0), t = gpu._interactionRoot->GetTRootParamIndex(0),
			           u = gpu._interactionRoot->GetURootParamIndex(0);
			add("grass interaction version " + std::to_string(gpu._interactionRevision),
			    {access(frame.Sources.get(), Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ),
			     access(frame.InteractionConstants.get(), Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ),
			     access(gpu._trail.get(), Mode::ReadWrite, UnorderedAccess),
			     access(gpu._previousTrail.get(), Mode::Write, UnorderedAccess),
			     access(gpu._presence.get(), Mode::Write, UnorderedAccess)},
			    [=](auto* cmd)
			    {
				    cmd->SetDescriptorHeaps(1, &heap);
				    cmd->SetComputeRootSignature(root.Get());
				    cmd->SetPipelineState(pipeline.Get());
				    cmd->SetComputeRootConstantBufferView(b, cb);
				    cmd->SetComputeRootDescriptorTable(t, sources);
				    for (UINT i = 0; i < 3; ++i)
				    {
					    cmd->SetComputeRootDescriptorTable(u + i, uavs[i]);
				    }
				    cmd->Dispatch(8, 8, 1);
			    });
		}
		if (afterInteraction)
		{
			afterInteraction();
		}
		for (const auto& job : frame.WindJobs)
		{
			const auto root = gpu._windRoot->GetRootSignature();
			const auto pipeline = gpu._windPSO.at(job.Stage);
			const auto cb = job.Constants->GetElementAddress(0);
			const auto output = job.Output->GetUAV()->GPUHandle;
			const auto input = (job.Input ? job.Input : gpu._neutralWind.get())->GetSRV()->GPUHandle;
			const auto pressure = (job.Pressure ? job.Pressure : gpu._neutralWind.get())->GetSRV()->GPUHandle;
			const UINT b = gpu._windRoot->GetBRootParamIndex(0), t = gpu._windRoot->GetTRootParamIndex(0),
			           u = gpu._windRoot->GetURootParamIndex(0), groups = (job.Resolution + 7) / 8;
			std::vector<Access> accesses = {access(job.Constants, Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ),
			                                access(job.Output, Mode::ReadWrite, UnorderedAccess)};
			if (job.Input)
			{
				accesses.push_back(access(job.Input, Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
			}
			if (job.Pressure)
			{
				accesses.push_back(access(job.Pressure, Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
			}
			add("grass wind stage " + std::to_string(job.Stage), accesses,
			    [=](auto* cmd)
			    {
				    cmd->SetDescriptorHeaps(1, &heap);
				    cmd->SetComputeRootSignature(root.Get());
				    cmd->SetPipelineState(pipeline.Get());
				    cmd->SetComputeRootConstantBufferView(b, cb);
				    cmd->SetComputeRootDescriptorTable(t, input);
				    cmd->SetComputeRootDescriptorTable(t + 1, pressure);
				    cmd->SetComputeRootDescriptorTable(u, output);
				    cmd->Dispatch(groups, groups, 1);
			    });
		}
	}
	if (!recorded.HiZ)
	{
		recorded.HiZ = std::make_unique<GDX12UploadBuffer<float>>(_resources.Device, 1, EBufferType::Default, false);
		recorded.HiZ->CreateSRV(_resources.SRV_UAV_Heap.get(),
		                        _resources.SRV_UAV_Heap->GetAvailableIndex(SceneDescriptorStart));
		recorded.HiZ->CreateUAV(_resources.SRV_UAV_Heap.get(),
		                        _resources.SRV_UAV_Heap->GetAvailableIndex(SceneDescriptorStart));
	}
	if (!viewsOnly && !targets.empty())
	{
		recorded.Built.resize(gpu.PatchCount());
		for (UINT patch = 0; patch < gpu.PatchCount(); ++patch)
		{
			gpu.PrepareView(targets.front().View, patch, &materials, false);
			const auto* output = gpu.FindOutput(targets.front().View, patch);
			if (!output)
			{
				continue;
			}
			const auto& source = gpu._grass.at(output->Owner).Source;
			auto& built = recorded.Built[patch];
			GrassLocalBounds(source, false).Transform(built.Bounds, source.World);
			const UINT ribbons = source.Implementation == GDX12GrassImplementation::CrossedRibbons ? 2u : 1u;
			const UINT stride =
			    (source.Segments + 1) * 2 * ribbons + ((std::max)(1u, source.Segments / 2) + 1) * 2 * ribbons + 4;
			const UINT64 count = UINT64(GrassBladeCount(source)) * stride;
			if (count > UINT_MAX / sizeof(GDX12GrassRenderer::Vertex))
			{
				throw std::length_error("Built grass vertices exceed buffer size limit");
			}
			if (!built.Vertices || built.Vertices->GetElementCount() < count)
			{
				built.Vertices = gpu.Buffer<GDX12GrassRenderer::Vertex>(UINT(count), true, true);
			}
			const auto root = _geometryRoot->GetRootSignature();
			const auto pipeline = _buildGeometry;
			const auto cb = output->Constants->GetElementAddress(output->ConstantIndex);
			const auto roots = gpu._grass.at(output->Owner).Roots[slot]->GetSRV()->GPUHandle;
			const auto wind = output->CurrentWind->GetSRV()->GPUHandle,
			           previousWind = output->PreviousWind->GetSRV()->GPUHandle;
			const auto trail = gpu._trail->GetSRV()->GPUHandle, previousTrail = gpu._previousTrail->GetSRV()->GPUHandle;
			const auto vertices = built.Vertices->GetUAV()->GPUHandle;
			const UINT b = _geometryRoot->GetBRootParamIndex(0), t = _geometryRoot->GetTRootParamIndex(0),
			           u = _geometryRoot->GetURootParamIndex(0);
			const UINT groups = (GrassBladeCount(source) + 63) / 64;
			add("grass patch " + std::to_string(patch) + " build all LOD vertices",
			    {access(output->Constants, Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ),
			     access(gpu._grass.at(output->Owner).Roots[slot].get(), Mode::Read,
			            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
			     access(output->CurrentWind, Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
			     access(output->PreviousWind, Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
			     access(gpu._trail.get(), Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
			     access(gpu._previousTrail.get(), Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
			     access(built.Vertices.get(), Mode::Write, UnorderedAccess)},
			    [=](auto* cmd)
			    {
				    cmd->SetDescriptorHeaps(1, &heap);
				    cmd->SetComputeRootSignature(root.Get());
				    cmd->SetPipelineState(pipeline.Get());
				    cmd->SetComputeRootConstantBufferView(b, cb);
				    const D3D12_GPU_DESCRIPTOR_HANDLE inputs[] = {roots, wind, previousWind, trail, previousTrail};
				    for (UINT i = 0; i < 5; ++i)
				    {
					    cmd->SetComputeRootDescriptorTable(t + i, inputs[i]);
				    }
				    cmd->SetComputeRootDescriptorTable(u, vertices);
				    cmd->Dispatch(groups, 1, 1);
			    });
		}
	}
	bool hiZReady = false;
	std::vector<bool> mainExpanded(gpu.PatchCount(), false);
	for (const auto& target : targets)
	{
		if (target.View.Stage == GDX12GeometryStage::GBuffer && afterDepth)
		{
			afterDepth();
		}
		XMFLOAT4X4 clip;
		XMStoreFloat4x4(&clip, target.View.ViewProjection.Transpose());
		for (UINT patch = 0; patch < gpu.PatchCount(); ++patch)
		{
			{
				if (!LocalShadowFaceIntersects(clip, recorded.Built.at(patch).Bounds))
				{
					continue;
				}
			}
			gpu.PrepareView(target.View, patch, &materials, false);
			const auto* output = gpu.FindOutput(target.View, patch);
			if (!output)
			{
				continue;
			}
			const bool mainPair = !viewsOnly && target.View.Key == 1;
			auto* vertices = recorded.Built.at(patch).Vertices.get();
			auto* indices = gpu._indices.get();
			auto* arguments = gpu._arguments.get();
			if (mainPair && patch)
			{
				if (recorded.MainPatches.size() < gpu.PatchCount())
				{
					recorded.MainPatches.resize(gpu.PatchCount());
				}
				auto& buffers = recorded.MainPatches[patch];
				if (!buffers.Indices || buffers.Indices->GetElementCount() < indices->GetElementCount())
				{
					buffers.Indices = gpu.Buffer<UINT>(indices->GetElementCount(), false, true);
				}
				if (!buffers.Arguments)
				{
					buffers.Arguments = gpu.Buffer<UINT>(8, false, true);
				}
				indices = buffers.Indices.get();
				arguments = buffers.Arguments.get();
			}
			const bool reuse = mainPair && target.View.Stage == GDX12GeometryStage::GBuffer;
			if (reuse && !mainExpanded[patch])
			{
				throw std::logic_error("Grass GBuffer requires its matching depth culling");
			}
			if (mainPair && !hiZReady && target.View.SceneDepth)
			{
				UINT64 total = 0;
				for (UINT w = target.Width, h = target.Height;; w = (w + 1) / 2, h = (h + 1) / 2)
				{
					total += UINT64(w) * h;
					if (w == 1 && h == 1)
					{
						break;
					}
				}
				if (total > UINT_MAX / sizeof(float))
				{
					throw std::length_error("Grass HiZ exceeds buffer size limit");
				}
				recorded.HiZ->Resize(UINT(total));
				const auto root = _hiZRoot->GetRootSignature();
				const auto pipeline = _buildHiZ;
				const auto depth = target.View.SceneDepth->GetSRV()->GPUHandle, hiz = recorded.HiZ->GetUAV()->GPUHandle;
				const UINT t = _hiZRoot->GetTRootParamIndex(0), u = _hiZRoot->GetURootParamIndex(0);
				UINT sourceWidth = 0, sourceHeight = 0, sourceOffset = 0, offset = 0;
				for (UINT w = target.Width, h = target.Height;; w = (w + 1) / 2, h = (h + 1) / 2)
				{
					const std::array<UINT, 6> constants = {w, h, sourceWidth, sourceHeight, sourceOffset, offset};
					add("grass opaque depth HiZ " + std::to_string(w) + "x" + std::to_string(h),
					    {{target.Depth, Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE},
					     access(recorded.HiZ.get(), Mode::ReadWrite, UnorderedAccess)},
					    [=](auto* cmd)
					    {
						    cmd->SetDescriptorHeaps(1, &heap);
						    cmd->SetComputeRootSignature(root.Get());
						    cmd->SetPipelineState(pipeline.Get());
						    cmd->SetComputeRoot32BitConstants(0, 6, constants.data(), 0);
						    cmd->SetComputeRootDescriptorTable(t, depth);
						    cmd->SetComputeRootDescriptorTable(u, hiz);
						    cmd->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
					    });
					if (w == 1 && h == 1)
					{
						break;
					}
					sourceWidth = w;
					sourceHeight = h;
					sourceOffset = offset;
					offset += w * h;
				}
				hiZReady = true;
			}
			const auto& state = gpu._grass.at(output->Owner);
			auto* computeRoot = _geometryRoot.get();
			const auto root = computeRoot->GetRootSignature();
			const auto clear = _clearGeometry, expand = _cullGeometry;
			const auto cb = output->Constants->GetElementAddress(output->ConstantIndex);
			const std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 5> srvs = {
			    state.Roots[slot]->GetSRV()->GPUHandle, output->CurrentWind->GetSRV()->GPUHandle,
			    output->PreviousWind->GetSRV()->GPUHandle, gpu._trail->GetSRV()->GPUHandle,
			    gpu._previousTrail->GetSRV()->GPUHandle};
			const std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 3> uavs = {
			    vertices->GetUAV()->GPUHandle, indices->GetUAV()->GPUHandle, arguments->GetUAV()->GPUHandle};
			const UINT b = computeRoot->GetBRootParamIndex(0), t = computeRoot->GetTRootParamIndex(0),
			           u = computeRoot->GetURootParamIndex(0), groups = (GrassBladeCount(state.Source) + 63) / 64;
			const auto hizSrv = recorded.HiZ->GetSRV()->GPUHandle;
			const auto bind = [=](ID3D12GraphicsCommandList* cmd)
			{
				cmd->SetDescriptorHeaps(1, &heap);
				cmd->SetComputeRootSignature(root.Get());
				cmd->SetComputeRootConstantBufferView(b, cb);
				for (UINT i = 0; i < 5; ++i)
				{
					cmd->SetComputeRootDescriptorTable(t + i, srvs[i]);
				}
				for (UINT i = 0; i < 3; ++i)
				{
					cmd->SetComputeRootDescriptorTable(u + i, uavs[i]);
				}
				cmd->SetComputeRootDescriptorTable(t + 5, hizSrv);
			};
			std::vector<Access> accesses = {
			    access(output->Constants, Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ),
			    access(state.Roots[slot].get(), Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
			    access(output->CurrentWind, Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
			    access(output->PreviousWind, Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
			    access(gpu._trail.get(), Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
			    access(gpu._previousTrail.get(), Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
			    access(vertices, Mode::Read, State(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)),
			    access(indices, Mode::Write, UnorderedAccess),
			    access(arguments, Mode::ReadWrite, UnorderedAccess)};
			accesses.push_back(access(recorded.HiZ.get(), Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
			const std::string name =
			    "grass view " + std::to_string(target.View.Key) + " patch " + std::to_string(patch);
			if (!reuse)
			{
				add(name + " clear arguments", accesses,
				    [=](auto* cmd)
				    {
					    bind(cmd);
					    cmd->SetPipelineState(clear.Get());
					    cmd->Dispatch(1, 1, 1);
				    });
				add(name + " cull indices", accesses,
				    [=](auto* cmd)
				    {
					    bind(cmd);
					    cmd->SetPipelineState(expand.Get());
					    cmd->Dispatch(groups, 1, 1);
				    });
				if (mainPair)
				{
					mainExpanded[patch] = true;
				}
			}
			auto binding = output->Binding;
			auto batch = output->Batch;
			binding.SRVs.front().second = vertices->GetSRV()->GPUHandle;
			batch.IndexBuffer.BufferLocation = indices->GetElementAddress(0);
			batch.IndexBuffer.SizeInBytes = UINT(indices->GetElementCount() * sizeof(UINT));
			batch.Draw.Arguments = arguments->GetResource().D3DResource.Get();
			std::vector<Access> draw = {
			    access(vertices, Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
			    access(indices, Mode::Read, D3D12_RESOURCE_STATE_INDEX_BUFFER),
			    access(arguments, Mode::Read, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT),
			    access(output->Constants, Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ),
			    {2, Mode::Read,
			     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE},
			    {target.Depth, target.View.Stage == GDX12GeometryStage::GBuffer ? Mode::Read : Mode::Write,
			     State(target.View.Stage == GDX12GeometryStage::GBuffer ? D3D12_RESOURCE_STATE_DEPTH_READ
			                                                            : D3D12_RESOURCE_STATE_DEPTH_WRITE)}};
			draw.insert(draw.end(), target.AdditionalReads.begin(), target.AdditionalReads.end());
			for (auto color : target.Colors)
			{
				draw.push_back({color, Mode::Write, D3D12_RESOURCE_STATE_RENDER_TARGET});
			}
			draw.push_back(
			    access(_resources.FrameConstants[slot]->MainCB.get(), Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ));
			if (target.Camera != Invalid)
			{
				D3D12_HEAP_PROPERTIES heapType{};
				ThrowIfFailed(all.at(target.Camera)->GetHeapProperties(&heapType, nullptr));
				draw.push_back({target.Camera, Mode::Read,
				                heapType.Type == D3D12_HEAP_TYPE_UPLOAD
				                    ? State(D3D12_RESOURCE_STATE_GENERIC_READ)
				                    : State(D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER)});
			}
			add(name + " ExecuteIndirect", draw,
			    [=](auto* cmd)
			    {
				    cmd->SetDescriptorHeaps(1, &heap);
				    cmd->SetGraphicsRootSignature(binding.Root->GetRootSignature().Get());
				    cmd->SetPipelineState(binding.Pipeline.Get());
				    for (const auto& cbv : binding.CBVs)
				    {
					    cmd->SetGraphicsRootConstantBufferView(binding.Root->GetBRootParamIndex(cbv.first), cbv.second);
				    }
				    for (const auto& srv : binding.SRVs)
				    {
					    cmd->SetGraphicsRootDescriptorTable(binding.Root->GetTRootParamIndex(srv.first), srv.second);
				    }
				    D3D12_VIEWPORT vp{
				        float(target.X), float(target.Y), float(target.Width), float(target.Height), 0, 1};
				    D3D12_RECT rect{LONG(target.X), LONG(target.Y), LONG(target.X + target.Width),
				                    LONG(target.Y + target.Height)};
				    cmd->RSSetViewports(1, &vp);
				    cmd->RSSetScissorRects(1, &rect);
				    cmd->OMSetRenderTargets(UINT(target.Rtvs.size()), target.Rtvs.data(), FALSE, &target.Dsv);
				    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
				    cmd->IASetIndexBuffer(&batch.IndexBuffer);
				    cmd->ExecuteIndirect(batch.Draw.Signature, 1, batch.Draw.Arguments, 0, nullptr, 0);
			    });
		}
	}
}

bool SceneGrass::Record(UINT slot, Id id, ID3D12GraphicsCommandList* cmd) const
{
	const auto& commands = _frames.at(slot).Commands;
	const auto found = std::lower_bound(commands.begin(), commands.end(), id,
	                                    [](const auto& entry, Id operation) { return entry.first < operation; });
	if (found == commands.end() || found->first != id)
	{
		return false;
	}
	found->second(cmd);
	return true;
}

} // namespace BistroRender

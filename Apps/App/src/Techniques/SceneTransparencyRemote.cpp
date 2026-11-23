#include "SceneTransparency.h"
#include "ProbeInputMirror.h"
#include "WboitMeshDraw.h"
#include "WboitTransfer.h"
#include "Common/ConsoleVariables.h"
#include "Engine.Core/Logging/Logger.h"
#include "Common/GameTimer.h"

namespace BistroRender
{
namespace
{
ID3D12Resource* Upload(GDX12Device& device, GDX12ResourceHandle& buffer, const void* data, UINT64 bytes)
{
	if (!buffer || buffer->GetDesc().Width < bytes)
		buffer = device.CreateResource(D3D12_HEAP_TYPE_UPLOAD, CD3DX12_RESOURCE_DESC::Buffer(bytes),
		    D3D12_RESOURCE_STATE_GENERIC_READ);
	void* mapped = nullptr;
	const D3D12_RANGE noRead{};
	ThrowIfFailed(buffer->Map(0, &noRead, &mapped));
	std::memcpy(mapped, data, size_t(bytes));
	buffer->Unmap(0, nullptr);
	return buffer.Get();
}

struct TransparentGeometry
{
	GDX12GeometryBuffer::RecordingBinding Binding{};
	std::map<std::tuple<UINT, UINT, INT>, std::pair<UINT, INT>> Offsets;
};

bool WboitVisible(const std::array<XMVECTOR, 6>& planes, const GDX12InstanceData& instance)
{
	const auto center = XMVectorSetW(XMLoadFloat3(&instance.BoundingBoxCenter), 1);
	const auto extents = XMLoadFloat3(&instance.BoundingBoxExtents);
	for (const auto plane : planes)
	{
		const float distance = XMVectorGetX(XMVector4Dot(center, plane));
		const float radius = XMVectorGetX(XMVector3Dot(extents, XMVectorAbs(plane)));
		if (distance + radius < 0) return false;
	}
	return true;
}

AutoConsoleVariable<int> WboitAdapter(L"Render.WBOIT.Adapter", -1, L"DXGI adapter for exact Bistro WBOIT accumulation; -1 uses GPU0.",
                                      EConsoleVariableFlags::Default, [](int index) { return index >= -1; });
Id PacketFor(const Schedule& plan, Id operation)
{
	auto it = std::find_if(plan.Packets.begin(), plan.Packets.end(), [=](const auto& p){ return p.Operation == operation; });
	if (it == plan.Packets.end()) throw std::logic_error("WBOIT packet is missing");
	return Id(it - plan.Packets.begin());
}
} // namespace

struct SceneTransparency::Shared
{
	Shared(GDX12Device& device, size_t slots)
	{
		SimulationResources.Device = &device;
		SimulationResources.FrameConstants.resize(slots);
		SimulationResources.SRV_UAV_Heap = std::make_shared<GDX12DescriptorHeap>(&device,
		    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, SceneDescriptorStart + 4096, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
	}
	GDX12DeviceResources SimulationResources;
	GDX12ParticleSimulation Simulation;
	Id Owner;
	uint64_t MeshRevision = UINT64_MAX;
	std::shared_ptr<TransparentGeometry> Geometry;
	bool Scheduled = false, ReportedFirst = false, ReportedRegular = false;
};
struct SceneTransparency::Remote
{
	Remote(const std::shared_ptr<QueueSubmission>& submit, Id owner, RenderPipelineCommonData& common, const std::shared_ptr<Shared>& shared)
	    : GPU(submit, owner, 1), DepthMirror(*submit, owner), SharedInputs(shared), Owner(owner)
	{
		Resources.Initialize(&submit->Device(owner));
		Draw.Setup(&Resources, &common);
		Draw.SetFlag(RENDER_PASS_FLAG_USE_DOWNSCALED_RESOLUTION, true);
		Draw.SetUnbatchedDraws(true);
		Draw.SetSimpleLighting(true);
		Compose.Setup(&Resources, &common);
		Compose.SetFlag(RENDER_PASS_FLAG_USE_DOWNSCALED_RESOLUTION, true);
	}
	~Remote()
	{
		try { if (Consumer) GPU.Submissions()->Wait(Consumer); GPU.Drain(); } catch (...) {}
		Compose.ClearDenendencies();
		Draw.ClearDenendencies();
	}
	Executor GPU;
	ProbeInputMirror DepthMirror;
	NativeWork Early;
	Id DepthExport = Invalid;
	ID3D12Resource* DepthResource = nullptr;
	std::shared_ptr<Shared> SharedInputs;
	Id Owner;
	GDX12DeviceResources Resources;
	SceneParticles::RemoteRaster Particles;
	GDX12WBOITTransparencyPass Draw;
	GDX12WBOITCompositionPass Compose;
	GDX12Texture Depth;
	std::array<GDX12Descriptor, 3> SceneViews;
	std::shared_ptr<TransparentGeometry> Geometry;
	std::array<GDX12ResourceHandle, 6> FrameInputs;
	std::array<GDX12ResourceHandle, 2> GeometryUploads;
	std::unique_ptr<WboitResultTransfer> Transfer;
	Graph Plan;
	NativeWork Work;
	std::vector<ID3D12Resource*> Bindings;
	Id Import = Invalid, OwnerResolve = Invalid, Mesh = Invalid, Apply = Invalid;
	CompletionToken MeshDone, Resolved, Produced, Imported, Applied, Consumer;
	UINT64 Frame = 0, DepthBytes = 0, GeometryBytes = 0;
	UINT Width = 0, Height = 0, TransparentDraws = 0;
	std::array<UINT, 3> EmitterCounts{};
	bool Bootstrap = false;

	void Report()
	{
		if (!Applied || !Consumer || !GPU.Submissions()->Complete(Consumer)) return;
		auto& reported = Bootstrap ? SharedInputs->ReportedFirst : SharedInputs->ReportedRegular;
		if (reported) return;
		reported = true;
		Log::Info("RenderGraph", "WBOIT Exact completed frame=", Frame, " owner=", GPU.Submissions()->AdapterLabel(Owner),
		    " display=", GPU.Submissions()->AdapterLabel(0), " raster=mesh+particles resolve=owner sorted/additive=owner",
		    " primary=layer-application dimensions=", Width, "x", Height, " accum=RGBA16_FLOAT reveal=R16_FLOAT hdr=RGBA16_FLOAT",
		    " motion=0",
		    " weightedEmitters=", EmitterCounts[0], " sortedEmitters=", EmitterCounts[1], " additiveEmitters=", EmitterCounts[2],
		    " transparentSceneCommands=", TransparentDraws, " inputBytes=", DepthBytes, " depthSource=opaque-prepass depthBytes=", DepthBytes,
		    " opaqueHdrBytes=0 shadowBytes=0 particlePoolBytes=0 hostGeometryUploadBytes=", GeometryBytes,
		    " layerBytes=", Transfer->Bytes, " finalResultBytes=", Transfer->Bytes, " ownerMeshFence=", MeshDone.Value, " ownerResolveFence=", Resolved.Value, " ownerExportFence=", Produced.Value,
		    " primaryImportFence=", Imported.Value, " primaryApplyFence=", Applied.Value, " completedFrameFence=", Consumer.Value);
	}
};

Id SceneTransparency::Begin(const std::shared_ptr<QueueSubmission>& submit, UINT slot, uint64_t frame)
{
	_frame = frame;
	_work.Commands.clear();
	Id owner = submit->SelectAdapter(WboitAdapter.GetValue());
	constexpr unsigned SkinnedTransparent = 1, InteractionFieldEmitter = 2;
	unsigned fallback = 0;
	if (owner)
	{
		const auto& constants = *_resources.FrameConstants.at(slot);
		const auto* cmds = _resources.IndirectCommandsCache->GetCPUData();
		const auto* instances = constants.InstanceCache->GetCPUData();
		const auto* materials = constants.MaterialCache->GetCPUData();
		for (UINT i = 0; i < _resources.IndirectCommandsCache->GetElementCount(); ++i)
		{
			if (!cmds[i].InstanceCount || !cmds[i].IndexCountPerInstance) continue;
			const auto& instance = instances[cmds[i].InstanceID];
			const auto& material = materials[instance.MaterialIndex];
			if (instance.SkinOutputVertex != UINT_MAX && material.RenderLayer == UINT(MaterialType::Transparent) &&
			    !material.ReflectionProbePreview)
			{
				fallback |= SkinnedTransparent;
				break;
			}
		}
		if (std::any_of(_resources.ParticleEmitters.begin(), _resources.ParticleEmitters.end(), [](const auto& emitter) {
			    return emitter.Blend != ParticleBlend::Opaque && emitter.InteractionEnabled;
		    }))
			fallback |= InteractionFieldEmitter;
		if (fallback && fallback != _fallbackReasons)
			Log::Warning("RenderGraph", "WBOIT uses local rendering on ", submit->AdapterLabel(0),
			    " because the remote path requires rigid transparent meshes and depth-only particle inputs",
			    " skinnedTransparent=", bool(fallback & SkinnedTransparent), " interactionFieldEmitter=", bool(fallback & InteractionFieldEmitter));
		if (fallback) owner = 0;
	}
	_fallbackReasons = fallback;
	const Id previousOwner = _shared ? _shared->Owner : 0;
	if (owner != previousOwner)
	{
		if (_shared)
			_particles.PreserveConsumedBursts(_shared->Simulation);
		else if (_resources.ParticleSimulation)
			_particles.PreserveConsumedBursts(*_resources.ParticleSimulation);
	}
	auto& entry = _remoteSlots.at(slot);
	if (entry)
	{
		entry->Report(); // the primary executor already retired this frame slot
		if (owner != entry->Owner) entry.reset();
	}
	if (!owner) _shared.reset();
	if (owner && (!_shared || _shared->Owner != owner))
	{
		_shared = std::make_shared<Shared>(submit->Device(owner), _remoteSlots.size());
		_shared->Owner = owner;
	}
	if (owner && (!entry || entry->SharedInputs != _shared))
		entry = std::make_unique<Remote>(submit, owner, _common, _shared);
	_remote = entry.get();
	return owner;
}
void SceneTransparency::PrepareOpaqueDepth(Graph& primary, std::vector<ID3D12Resource*>& bindings,
    UINT slot, GDX12Texture* depth)
{
	auto* source = depth->GetResource()->D3DResource.Get();
	if (!_remote)
	{
		auto& frame = _frames.at(slot);
		frame.RasterDepth = depth;
		if (!_water.DrawsDiagnosticParticles()) return;

		// The later water draw writes the shared depth. Keep the version used by remote WBOIT.
		auto& snapshot = frame.LocalDepthSnapshot;
		if (!snapshot.IsInitialized() || snapshot.GetWidth() != depth->GetWidth() ||
		    snapshot.GetHeight() != depth->GetHeight())
		{
			GDX12TextureDesc desc;
			desc.Width = depth->GetWidth();
			desc.Height = depth->GetHeight();
			desc.Format = depth->GetFormat();
			desc.SRV_UAV_Heap = _resources.SRV_UAV_Heap.get();
			desc.SRVDesc = depth->ShaderViewDescription();
			desc.SRVHeapIndex = snapshot.IsInitialized() ? snapshot.GetSRV()->HeapIndex
			    : desc.SRV_UAV_Heap->GetAvailableIndex(SceneDescriptorStart);
			desc.ExternalResource = _resources.Device->CreateResource(D3D12_HEAP_TYPE_DEFAULT, source->GetDesc(),
			    D3D12_RESOURCE_STATE_COMMON);
			snapshot.Initialize(desc);
		}
		auto* destination = snapshot.GetResource()->D3DResource.Get();
		const Id sourceId = NativeWork::ResourceFor(primary, bindings, source, "opaque prepass depth");
		const Id snapshotId = NativeWork::ResourceFor(primary, bindings, destination, "local WBOIT opaque depth snapshot");
		_work.Add(primary, "save completed opaque depth for local WBOIT",
		    {{sourceId, Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE},
		     {snapshotId, Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST}},
		    [source, destination](auto* cmd) { cmd->CopyResource(destination, source); });
		frame.RasterDepth = &snapshot;
		return;
	}
	auto& r = *_remote;
	r.Early.Commands.clear();
	r.DepthMirror.Begin();
	r.DepthMirror.RetireUnused({source}, [](auto*){});
	const Id sourceId = NativeWork::ResourceFor(primary, bindings, source, "opaque prepass depth");
	r.DepthResource = r.DepthMirror.Mirror(primary, bindings, source, sourceId, true, {}, {}, nullptr, true);
	r.DepthExport = r.DepthMirror.AppendExport(primary, bindings, r.Early, Queue::Direct);
	primary.Operations[r.DepthExport].Name = "GPU0 export completed opaque prepass depth for WBOIT";
	r.DepthBytes = r.DepthMirror.PendingTransferBytes(source);
}
bool SceneTransparency::RecordPrepass(Id op, ID3D12GraphicsCommandList* cmd) const
{
	return _remote && _remote->Early.Record(op, cmd);
}
void SceneTransparency::AddExchange(const Schedule& primary, std::vector<Executor::PacketExchange>& exchanges)
{
	if (!_remote) return;
	auto& remote = *_remote;
	auto schedule = Compile(remote.Plan);
	const Id input = PacketFor(schedule, 0);
	const Id resolve = PacketFor(schedule, remote.OwnerResolve), mesh = PacketFor(schedule, remote.Mesh);
	remote.Apply = PacketFor(primary, remote.Apply);
	remote.Import = PacketFor(primary, remote.Import);
	remote.GPU.Begin(0);
	remote.GPU.Reserve(schedule);
	exchanges.push_back({PacketFor(primary, remote.DepthExport), remote.Import,
	    [&remote, schedule = std::move(schedule), input, resolve, mesh](CompletionToken inputs)
	    {
		    remote.GPU.Execute(schedule, remote.Bindings, [&remote](Id op, UINT, ID3D12GraphicsCommandList* cmd)
		    {
			    if (!remote.Work.Record(op, cmd)) throw std::logic_error("Missing WBOIT owner work");
		    }, nullptr, nullptr, nullptr, {{input, inputs}});
		    remote.MeshDone = remote.GPU.Completions().at(mesh);
		    remote.Resolved = remote.GPU.Completions().at(resolve);
		    return remote.Produced = remote.GPU.Completions().back();
	    }});
}
void SceneTransparency::Submitted(const std::vector<CompletionToken>& completions)
{
	if (!_remote || completions.empty()) return;
	if (_remote->Import < completions.size()) _remote->Imported = completions[_remote->Import];
	if (_remote->Apply < completions.size()) _remote->Applied = completions[_remote->Apply];
	_remote->Consumer = completions.back();
}
void SceneTransparency::Presented()
{
	if (_remote) _remote->Report();
}


SceneTransparency::SceneTransparency(GDX12DeviceResources& resources, RenderPipelineCommonData& common)
    : _remoteSlots(resources.FrameConstants.size()), _resources(resources), _common(common), _frames(resources.FrameConstants.size()), _particles(resources, common),
      _water(resources, common)
{
	for (auto& frame : _frames)
	{
		frame.Draw.SetFlag(RENDER_PASS_FLAG_USE_DOWNSCALED_RESOLUTION, true);
		frame.Draw.SetUnbatchedDraws(true);
		frame.Draw.SetSimpleLighting(true);
		frame.Compose.SetFlag(RENDER_PASS_FLAG_USE_DOWNSCALED_RESOLUTION, true);
		frame.Compose.SetApplyOpaqueAerialPerspective(true);
	}
}
SceneTransparency::~SceneTransparency()
{
	_remoteSlots.clear();
	for (auto& frame : _frames)
	{
		frame.Apply.ClearDenendencies();
		frame.Compose.ClearDenendencies();
		frame.Draw.ClearDenendencies();
	}
}
void SceneTransparency::AppendRemote(SceneGeometry& geometry, UINT slot, Frame& frame, GDX12Texture* hdr)
{
	auto& r = *_remote;
	auto& submit = *r.GPU.Submissions();
	auto& plan = r.Plan;
	auto& work = r.Work;
	auto& bindings = r.Bindings;
	auto& constants = *_resources.FrameConstants.at(slot);
	auto* depth = geometry.Depth(slot);
	const auto native = [](GDX12Texture* t){ return t->GetResource()->D3DResource.Get(); };
	const auto id = [&](ID3D12Resource* value){ return NativeWork::ResourceFor(plan, bindings, value, "WBOIT owner input"); };
	plan = {};
	plan.Device = r.Owner;
	plan.FrameBoundary = false;
	bindings.clear();
	work.Commands.clear();
	r.Frame = _frame;
	r.Bootstrap = !r.SharedInputs->Scheduled;
	r.SharedInputs->Scheduled = true;
	r.Consumer = r.Applied = r.Imported = r.Produced = r.Resolved = r.MeshDone = {};
	auto* heap = r.Resources.SRV_UAV_Heap.get();
	const auto view = [&](GDX12Descriptor& d, ID3D12Resource* src, D3D12_SHADER_RESOURCE_VIEW_DESC desc){
		d.InitAsSRV(src, &desc, heap, d.HeapIndex == UINT_MAX ? heap->GetAvailableIndex(SceneDescriptorStart) : d.HeapIndex);
		return d.GPUHandle;
	};
	const auto wrap = [&](GDX12Texture& out, GDX12Texture* src, ID3D12Resource* resource){
		GDX12TextureDesc desc{};
		desc.Width = src->GetWidth();
		desc.Height = src->GetHeight();
		desc.Format = src->GetFormat();
		desc.ExternalResource = ComPtr<ID3D12Resource>(resource);
		desc.SRV_UAV_Heap = heap;
		desc.SRVDesc = src->ShaderViewDescription();
		desc.SRVHeapIndex = out.IsInitialized() ? out.GetSRV()->HeapIndex : heap->GetAvailableIndex(SceneDescriptorStart);
		out.Initialize(desc);
	};
	auto* ownerDepth = r.DepthResource;
	if (!ownerDepth || r.DepthExport == Invalid) throw std::logic_error("WBOIT missing opaque prepass snapshot");
	r.DepthMirror.AppendImport(plan, bindings, work);
	wrap(r.Depth, depth, ownerDepth);
	const bool resized = r.Width != hdr->GetWidth() || r.Height != hdr->GetHeight();
	const bool fresh = !r.Width;
	r.Width = hdr->GetWidth();
	r.Height = hdr->GetHeight();
	if (fresh) r.Draw.InitializeRaster(&r.Depth);
	else if (resized) r.Draw.Resize();
	else r.Draw.UpdateReadOnlyDepth();
	auto* accum = r.Draw.OUT_Accumulation.get();
	auto* reveal = r.Draw.OUT_Revealage.get();
	r.Compose.SetApplyOpaqueAerialPerspective(!_opaqueContinuation);
	r.Compose.SetMode(GDX12WBOITCompositionPass::Mode::ResolveLayer);
	r.Compose.SetInputs({accum, accum, reveal, &r.Depth});
	if (fresh) r.Compose.Initialize();
	else if (resized) r.Compose.Resize();
	auto* output = r.Compose.OUT_Result.get();
	if (!frame.ImportedLayer.IsInitialized())
		GDX12RenderHelpers::InitializeRenderTarget(frame.ImportedLayer, _resources, r.Width, r.Height, output->GetFormat());
	else if (frame.ImportedLayer.GetWidth() != r.Width || frame.ImportedLayer.GetHeight() != r.Height)
		frame.ImportedLayer.Resize(r.Width, r.Height);

	WboitMeshDraw mesh{};
	mesh.Root = r.Draw._transparencyRS.get();
	mesh.Pipeline = r.Draw._transparencyPSO.Get();
	mesh.Signature = r.Draw._transparencyCS.Get();
	mesh.Heap = heap->GetHeap().Get();
	mesh.Textures = heap->GetGPUHandle(Texture2D_StartIndex);
	mesh.Depth = r.Draw._readOnlyDepth->CPUHandle;
	mesh.Viewport = accum->GetViewport();
	mesh.Rect = accum->GetScissorRect();
	mesh.Targets = {accum->GetRTV()->CPUHandle, reveal->GetRTV()->CPUHandle};
	const State read = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
	std::vector<Access> accesses{{id(ownerDepth), Mode::Read, read | D3D12_RESOURCE_STATE_DEPTH_READ},
		{id(native(accum)), Mode::Write, D3D12_RESOURCE_STATE_RENDER_TARGET},
		{id(native(reveal)), Mode::Write, D3D12_RESOURCE_STATE_RENDER_TARGET}};
	const auto addRead = [&](ID3D12Resource* resource, State state = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE){
		const Id index = id(resource);
		auto found = std::find_if(accesses.begin(), accesses.end(), [=](auto a){ return a.Resource == index; });
		if (found == accesses.end()) accesses.push_back({index, Mode::Read, state});
		else found->Required |= state;
	};
	auto& device = *r.Resources.Device;
	const auto cpu = [&](UINT index, const void* data, UINT64 bytes){
		auto* target = Upload(device, r.FrameInputs[index], data, bytes);
		addRead(target, D3D12_RESOURCE_STATE_GENERIC_READ);
		return target;
	};
	const auto& main = constants.MainCB->GetCPUData()[0];
	const auto& camera = constants.CameraCB->GetCPUData()[_common.ActiveCameraCBufferIndex];
	mesh.Main = cpu(0, &main, sizeof(main))->GetGPUVirtualAddress();
	auto* ownerCamera = cpu(1, &camera, sizeof(camera));
	mesh.Camera = ownerCamera->GetGPUVirtualAddress();
	const auto structured = [&](UINT param, const auto& elements){
		using Element = typename std::decay_t<decltype(elements)>::value_type;
		auto* target = cpu(param + 2, elements.data(), UINT64(elements.size()) * sizeof(Element));
		D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
		desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
		desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		desc.Buffer.NumElements = UINT(elements.size());
		desc.Buffer.StructureByteStride = sizeof(Element);
		mesh.Views[param] = view(r.SceneViews[param], target, desc);
	};

	const auto& materials = *constants.MaterialCache;
	const auto* sourceInstances = constants.InstanceCache->GetCPUData();
	const auto* sourceTransforms = constants.TransformCache->GetCPUData();
	const auto* sourceDraws = _resources.IndirectCommandsCache->GetCPUData();
	std::vector<GDX12IndirectDrawArgs> meshSources;
	for (UINT i = 0; i < _resources.IndirectCommandsCache->GetElementCount(); ++i)
	{
		const auto& draw = sourceDraws[i];
		if (!draw.InstanceCount || !draw.IndexCountPerInstance) continue;
		const auto& instance = sourceInstances[draw.InstanceID];
		const auto& material = materials.GetCPUData()[instance.MaterialIndex];
		if (material.RenderLayer == UINT(MaterialType::Transparent) && !material.ReflectionProbePreview)
			meshSources.push_back(draw);
	}
	r.GeometryBytes = 0;
	r.GeometryUploads = {};
	const auto& geometryBuffer = *_resources.GeometryBuffer;
	const auto revision = geometryBuffer.GetMeshCacheRevision();
	auto& resident = r.SharedInputs->Geometry;
	const auto key = [](const auto& draw) {
		return std::make_tuple(draw.StartIndexLocation, draw.IndexCountPerInstance, draw.BaseVertexLocation);
	};
	if (!meshSources.empty())
	{
		if (!resident || r.SharedInputs->MeshRevision != revision ||
		    std::any_of(meshSources.begin(), meshSources.end(), [&](const auto& draw) { return resident->Offsets.find(key(draw)) == resident->Offsets.end(); }))
		{
			auto updated = std::make_shared<TransparentGeometry>();
			auto& binding = updated->Binding;
			const auto& sourceVertices = geometryBuffer.CPUVertices();
			const auto& sourceIndices = geometryBuffer.CPUIndices();
			std::vector<Vertex> vertices;
			std::vector<uint32_t> indexData;
			for (const auto& draw : meshSources)
			{
				if (!updated->Offsets.try_emplace(key(draw), UINT(indexData.size()), INT(vertices.size())).second) continue;
				const size_t indexEnd = size_t(draw.StartIndexLocation) + draw.IndexCountPerInstance;
				if (indexEnd > sourceIndices.size() || draw.BaseVertexLocation < 0)
					throw std::out_of_range("Invalid transparent mesh indices");
				const auto first = sourceIndices.begin() + draw.StartIndexLocation;
				const auto last = sourceIndices.begin() + indexEnd;
				const size_t vertexEnd = size_t(draw.BaseVertexLocation) + *std::max_element(first, last) + 1;
				if (vertexEnd > sourceVertices.size())
					throw std::out_of_range("Invalid transparent mesh vertices");
				vertices.insert(vertices.end(), sourceVertices.begin() + draw.BaseVertexLocation, sourceVertices.begin() + vertexEnd);
				indexData.insert(indexData.end(), first, last);
			}
			const UINT vertexBytes = UINT(vertices.size() * sizeof(Vertex));
			const UINT indexBytes = UINT(indexData.size() * sizeof(uint32_t));
			auto* vertexUpload = Upload(device, r.GeometryUploads[0], vertices.data(), vertexBytes);
			auto* indexUpload = Upload(device, r.GeometryUploads[1], indexData.data(), indexBytes);
			const auto residentBuffer = [&](UINT bytes) {
				return device.CreateResource(D3D12_HEAP_TYPE_DEFAULT, CD3DX12_RESOURCE_DESC::Buffer(bytes),
				    D3D12_RESOURCE_STATE_COMMON);
			};
			binding.VertexAllocation = residentBuffer(vertexBytes);
			binding.IndexAllocation = residentBuffer(indexBytes);
			auto* vertexBuffer = binding.VertexResource = binding.VertexAllocation.Get();
			auto* indexBuffer = binding.IndexResource = binding.IndexAllocation.Get();
			work.Add(plan, "upload transparent mesh geometry on owner",
			    {{id(vertexUpload), Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ},
			     {id(indexUpload), Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ},
			     {id(vertexBuffer), Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST},
			     {id(indexBuffer), Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST}},
			    [=](auto* cmd) {
				    cmd->CopyBufferRegion(vertexBuffer, 0, vertexUpload, 0, vertexBytes);
				    cmd->CopyBufferRegion(indexBuffer, 0, indexUpload, 0, indexBytes);
			    });
			binding.Vertices = {binding.VertexResource->GetGPUVirtualAddress(), vertexBytes, sizeof(Vertex)};
			binding.Indices = {binding.IndexResource->GetGPUVirtualAddress(), indexBytes, DXGI_FORMAT_R32_UINT};
			r.GeometryBytes = UINT64(vertexBytes) + indexBytes;
			resident = std::move(updated);
			r.SharedInputs->MeshRevision = revision;
		}
		r.Geometry = resident;
	}
	else r.Geometry.reset();

	// ViewProj is stored transposed for HLSL. Its CPU rows are the columns used in Culling.hlsl.
	const auto m = XMLoadFloat4x4(&camera.ViewProj);
	const std::array<XMVECTOR, 6> planes{m.r[3] + m.r[0], m.r[3] - m.r[0], m.r[3] + m.r[1],
	    m.r[3] - m.r[1], m.r[2], m.r[3] - m.r[2]};
	std::vector<GDX12IndirectDrawArgs> draws;
	std::vector<GDX12InstanceData> instances;
	std::vector<GDX12TransformConstants> transforms;
	std::vector<GDX12MaterialConstants> drawMaterials;
	std::unordered_set<UINT> indices;
	for (auto draw : meshSources)
	{
		auto instance = sourceInstances[draw.InstanceID];
		if (!WboitVisible(planes, instance)) continue;
		const auto& material = materials.GetCPUData()[instance.MaterialIndex];
		indices.insert(material.DiffuseIndex);
		draw.InstanceID = UINT(instances.size());
		transforms.push_back(sourceTransforms[instance.TransformIndex]);
		drawMaterials.push_back(material);
		instance.TransformIndex = instance.MaterialIndex = draw.InstanceID;
		const auto& [index, vertex] = r.Geometry->Offsets.at(key(draw));
		draw.StartIndexLocation = index;
		draw.BaseVertexLocation = vertex;
		draws.push_back(draw);
		instances.push_back(instance);
	}
	r.TransparentDraws = mesh.Draws = UINT(draws.size());
	if (mesh.Draws)
	{
		mesh.Geometry = r.Geometry->Binding;
		structured(0, drawMaterials);
		structured(1, transforms);
		structured(2, instances);
		mesh.Args = cpu(5, draws.data(), UINT64(draws.size()) * sizeof(draws.front()));
		addRead(mesh.Geometry.VertexResource, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		addRead(mesh.Geometry.IndexResource, D3D12_RESOURCE_STATE_INDEX_BUFFER);
	}

	auto& assetResources = r.SharedInputs->SimulationResources;
	const auto asset = [&](GDX12Texture& texture) {
		const auto source = std::find_if(_resources.Textures.begin(), _resources.Textures.end(),
		    [&](const auto& entry) { return entry.second.get() == &texture; });
		if (source == _resources.Textures.end()) throw std::logic_error("Unregistered WBOIT texture");
		auto found = assetResources.Textures.find(source->first);
		const auto loader = _resources.TextureReplicaLoaders.find(source->first);
		if (loader == _resources.TextureReplicaLoaders.end())
			throw std::logic_error("Missing replica source for WBOIT texture " + source->first);
		auto* copy = found == assetResources.Textures.end() ? loader->second.Load(assetResources) : found->second.get();
		auto* target = native(copy);
		NativeWork::ImportReadOnlyTexture(plan, bindings, *copy->GetResource(), read, "WBOIT texture replica");
		const auto desc = copy->ShaderViewDescription();
		const auto original = texture.ShaderViewDescription();
		if (desc.Format != original.Format || desc.Texture2D.MipLevels != original.Texture2D.MipLevels ||
		    copy->GetWidth() != texture.GetWidth() || copy->GetHeight() != texture.GetHeight())
			throw std::logic_error("WBOIT texture replica differs from the original: " + source->first);
		device.GetDevice()->CreateShaderResourceView(target, &desc, heap->GetCPUHandle(texture.GetSRV()->HeapIndex));
		return target;
	};
	for (const auto& [_, texture] : _resources.Textures)
	{
		const UINT index = texture->GetSRV()->HeapIndex;
		if (index >= Texture2D_StartIndex && indices.count(index - Texture2D_StartIndex)) addRead(asset(*texture));
	}
	r.Mesh = work.Add(plan, "WBOIT owner clear and draw transparent mesh", std::move(accesses), mesh);
	const Id cameraId = id(ownerCamera);
	_particles.AppendRemoteWeighted(plan, bindings, work, r.Resources, r.SharedInputs->SimulationResources,
	    r.SharedInputs->Simulation, accum, reveal, &r.Depth, cameraId, mesh.Camera, slot, r.Particles, asset);

	auto* root = r.Compose._compositionRS.get();
	auto* pipeline = r.Compose._compositionPSO.Get();
	const auto accumSrv = accum->GetSRV()->GPUHandle, revealSrv = reveal->GetSRV()->GPUHandle;
	const auto target = output->GetRTV()->CPUHandle;
	std::vector<Access> composition{{id(native(accum)), Mode::Read, read}, {id(native(reveal)), Mode::Read, read},
		{id(native(output)), Mode::Write, D3D12_RESOURCE_STATE_RENDER_TARGET}};
	r.OwnerResolve = work.Add(plan, "WBOIT owner premultiplied RGB and transmittance resolve", std::move(composition), [=](auto* cmd){
		cmd->SetDescriptorHeaps(1, &mesh.Heap);
		cmd->SetGraphicsRootSignature(root->GetRootSignature().Get());
		cmd->SetPipelineState(pipeline);
		cmd->SetGraphicsRootDescriptorTable(root->GetTRootParamIndex(1), accumSrv);
		cmd->SetGraphicsRootDescriptorTable(root->GetTRootParamIndex(2), revealSrv);
		cmd->RSSetViewports(1, &mesh.Viewport);
		cmd->RSSetScissorRects(1, &mesh.Rect);
		cmd->OMSetRenderTargets(1, &target, FALSE, nullptr);
		cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		cmd->DrawInstanced(3, 1, 0, 0);
	});
	_particles.AppendRemoteComposite(plan, bindings, work, output, r.Particles);
	if (!r.Transfer || resized) r.Transfer = std::make_unique<WboitResultTransfer>(submit, r.Owner, native(output));
	r.Transfer->Append(plan, bindings, work, native(output), true);
	if (r.SharedInputs->ReportedFirst && r.SharedInputs->ReportedRegular) return;
	r.EmitterCounts.fill(0);
	if (!r.Particles.Reads.empty())
		for (const auto& source : r.Particles.Frame.Sources)
		{
			if (source.Emitter.Blend == ParticleBlend::WeightedAlpha) ++r.EmitterCounts[0];
			if (source.Emitter.Blend == ParticleBlend::SortedAlpha) ++r.EmitterCounts[1];
			if (source.Emitter.Blend == ParticleBlend::Additive) ++r.EmitterCounts[2];
		}
}

GDX12Texture* SceneTransparency::ApplyRemote(Graph& primary, std::vector<ID3D12Resource*>& primaryBindings, SceneGeometry& geometry,
    UINT slot, Frame& frame, GDX12Texture* hdr, SceneEnvironment& environment)
{
	auto& r = *_remote;
	auto& scene = geometry._slots.at(slot);
	auto& constants = *_resources.FrameConstants.at(slot);
	const State read = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
	auto* depth = Depth();
	const auto native = [](GDX12Texture* t){ return t->GetResource()->D3DResource.Get(); };
	const auto primaryId = [&](ID3D12Resource* value){return NativeWork::ResourceFor(primary, primaryBindings, value, "WBOIT layer application");};
	r.Import = r.Transfer->Append(primary, primaryBindings, _work, native(&frame.ImportedLayer), false);
	primary.Operations[r.Import].Dependencies.push_back(r.DepthExport);
	auto& apply = frame.Apply;
	apply.Setup(&_resources, &_common);
	apply.SetFlag(RENDER_PASS_FLAG_USE_DOWNSCALED_RESOLUTION, true);
	apply.SetMode(GDX12WBOITCompositionPass::Mode::ApplyLayer);
	apply.SetApplyOpaqueAerialPerspective(!_opaqueContinuation);
	apply.SetInputs({hdr, &frame.ImportedLayer, depth});
	if (!apply.OUT_Result->IsInitialized()) apply.Initialize();
	else if (apply.OUT_Result->GetWidth() != r.Width || apply.OUT_Result->GetHeight() != r.Height) apply.Resize();
	const auto primaryRoot = apply._compositionRS.get();
	const auto primaryPipeline = apply._compositionPSO.Get();
	const auto displayHeap = _resources.SRV_UAV_Heap->GetHeap().Get();
	const auto primaryTarget = apply.OUT_Result->GetRTV()->CPUHandle;
	const auto camera = scene.Camera;
	const auto sky = constants.SkyCB->GetElementAddress(0);
	const std::array primaryViews{hdr->GetSRV()->GPUHandle, frame.ImportedLayer.GetSRV()->GPUHandle,
		frame.ImportedLayer.GetSRV()->GPUHandle, depth->GetSRV()->GPUHandle, _resources.SkyTransmittanceSRV.GPUHandle,
		_resources.SkyAerialRadianceSRV.GPUHandle, _resources.SkyAerialTransmittanceSRV.GPUHandle};
	const auto viewport = hdr->GetViewport();
	const auto scissor = hdr->GetScissorRect();
	std::vector<Access> display{{primaryId(native(hdr)), Mode::Read, read}, {primaryId(native(&frame.ImportedLayer)), Mode::Read, read},
		{primaryId(native(apply.OUT_Result.get())), Mode::Write, D3D12_RESOURCE_STATE_RENDER_TARGET}};
	if (!_opaqueContinuation)
	{
		environment.AddAerialReads(primary, primaryBindings, display);
		display.push_back({primaryId(native(depth)), Mode::Read, read});
		display.push_back({scene.FrameIds[1], Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ});
		display.push_back({primaryId(constants.SkyCB->GetResource().D3DResource.Get()), Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ});
	}
	r.Apply = _work.Add(primary, "GPU0 apply transparent layer to local HDR", std::move(display), [=](auto* cmd){
		cmd->SetDescriptorHeaps(1, &displayHeap);
		cmd->SetGraphicsRootSignature(primaryRoot->GetRootSignature().Get());
		cmd->SetPipelineState(primaryPipeline);
		cmd->SetGraphicsRootConstantBufferView(primaryRoot->GetBRootParamIndex(0), camera);
		cmd->SetGraphicsRootConstantBufferView(primaryRoot->GetBRootParamIndex(1), sky);
		for (UINT i = 0; i < primaryViews.size(); ++i)
			cmd->SetGraphicsRootDescriptorTable(primaryRoot->GetTRootParamIndex(i), primaryViews[i]);
		cmd->RSSetViewports(1, &viewport);
		cmd->RSSetScissorRects(1, &scissor);
		cmd->OMSetRenderTargets(1, &primaryTarget, FALSE, nullptr);
		cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		cmd->DrawInstanced(3, 1, 0, 0);
	});
	return apply.OUT_Result.get();
}

} // namespace BistroRender

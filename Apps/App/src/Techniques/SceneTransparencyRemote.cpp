#include "SceneTransparency.h"
#include "SceneLighting.h"
#include "ProbeInputMirror.h"
#include "WboitMeshDraw.h"
#include "WboitTransfer.h"
#include "WboitShadowInput.h"
#include "Common/ConsoleVariables.h"
#include "Engine.Core/Logging/Logger.h"
#include "Common/GameTimer.h"

namespace BistroRender
{
namespace
{
AutoConsoleVariable<int> WboitAdapter(L"App.WBOIT.Adapter", -1, L"DXGI adapter for exact Bistro WBOIT accumulation; -1 uses GPU0.",
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
	std::shared_ptr<ProbeInputMirror::Cache> Cache = std::make_shared<ProbeInputMirror::Cache>();
	Id Owner;
	uint64_t MeshRevision = UINT64_MAX, VertexRevision = 0, TextureRevision = UINT64_MAX;
	bool Scheduled = false, ReportedFirst = false, ReportedRegular = false;
};
struct SceneTransparency::Remote
{
	Remote(const std::shared_ptr<QueueSubmission>& submit, Id owner, RenderPipelineCommonData& common, const std::shared_ptr<Shared>& shared)
	    : GPU(submit, owner, 1), Mirror(*submit, owner, shared->Cache), DepthMirror(*submit, owner), SharedInputs(shared), Owner(owner)
	{
		Resources.Initialize(&submit->Device(owner));
		Draw.Setup(&Resources, &common);
		Draw.SetFlag(RENDER_PASS_FLAG_USE_DOWNSCALED_RESOLUTION, true);
		Draw.SetUnbatchedDraws(true);
		Draw.SetAtmosphereEnabled(true);
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
	ProbeInputMirror Mirror, DepthMirror;
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
	WboitShadowInput Sun, Local;
	std::array<GDX12Descriptor, 11> SceneViews;
	std::array<GDX12Descriptor, 3> SkyViews;
	SceneLighting::OwnerShadowInputs Shadows;
	std::shared_ptr<GDX12ResourceHandle> Skin;
	std::unique_ptr<WboitResultTransfer> Transfer;
	std::unordered_set<ID3D12Resource*> Live;
	Graph Plan;
	NativeWork Work;
	std::vector<ID3D12Resource*> Bindings;
	Id Export = Invalid, Import = Invalid, OwnerResolve = Invalid, Mesh = Invalid, Apply = Invalid;
	CompletionToken MeshDone, Resolved, Produced, Imported, Applied, Consumer;
	UINT64 Frame = 0, DepthBytes = 0, TextureBytes = 0, Inputs = 0, ShadowBytes = 0, OwnerShadowBytes = 0, GeometryBytes = 0;
	UINT Width = 0, Height = 0, TransparentDraws = 0;
	std::array<UINT, 3> EmitterCounts{};
	bool Motion = false, Bootstrap = false;

	void Report()
	{
		if (!Applied || !Consumer || !GPU.Submissions()->Complete(Consumer)) return;
		auto& reported = Bootstrap ? SharedInputs->ReportedFirst : SharedInputs->ReportedRegular;
		if (reported) return;
		reported = true;
		Log::Info("RenderGraph", "WBOIT Exact completed frame=", Frame, " owner=", GPU.Submissions()->AdapterLabel(Owner),
		    " display=", GPU.Submissions()->AdapterLabel(0), " raster=mesh+particles resolve=owner sorted/additive=owner",
		    " primary=layer-application dimensions=", Width, "x", Height, " accum=RGBA16_FLOAT reveal=R16_FLOAT hdr=RGBA16_FLOAT",
		    " motion=", Motion, " motionFormat=", Motion ? "RGBA32_FLOAT" : "none",
		    " weightedEmitters=", EmitterCounts[0], " sortedEmitters=", EmitterCounts[1], " additiveEmitters=", EmitterCounts[2],
		    " transparentSceneCommands=", TransparentDraws, " inputBytes=", Inputs, " depthSource=opaque-prepass depthBytes=", DepthBytes,
		    " opaqueHdrBytes=0 bindlessTextureBytes=", TextureBytes, " geometryBytes=", GeometryBytes,
		    " shadowBytes=", ShadowBytes, " ownerLocalShadowBytes=", OwnerShadowBytes,
		    " otherInputsAndPaddingBytes=", Inputs - DepthBytes - TextureBytes - GeometryBytes - ShadowBytes,
		    " layerBytes=", Transfer->LayerBytes, " finalResultBytes=", Transfer->Bytes, " ownerMeshFence=", MeshDone.Value, " ownerResolveFence=", Resolved.Value, " ownerExportFence=", Produced.Value,
		    " primaryImportFence=", Imported.Value, " primaryApplyFence=", Applied.Value, " completedFrameFence=", Consumer.Value);
	}
};

Id SceneTransparency::Begin(const std::shared_ptr<QueueSubmission>& submit, SceneLighting& lighting, UINT slot, uint64_t frame)
{
	_frame = frame;
	_lighting = &lighting;
	const Id owner = submit->SelectAdapter(WboitAdapter.GetValue());
	auto& entry = _remoteSlots.at(slot);
	if (entry)
	{
		entry->Report(); // the primary executor already retired this frame slot
		if (owner != entry->Owner) entry.reset();
	}
	if (!owner) _shared.reset();
	if (owner && (!_shared || _shared->Owner != owner))
	{
		_shared = std::make_shared<Shared>();
		_shared->Owner = owner;
	}
	if (owner && (!entry || entry->SharedInputs != _shared))
		entry = std::make_unique<Remote>(submit, owner, _common, _shared);
	_remote = entry.get();
	return owner;
}
void SceneTransparency::ExportPrepassDepth(Graph& primary, std::vector<ID3D12Resource*>& bindings, GDX12Texture* depth)
{
	if (!_remote) return;
	auto& r = *_remote;
	auto* source = depth->GetResource()->D3DResource.Get();
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
	exchanges.push_back({PacketFor(primary, remote.Export), remote.Import,
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
		frame.Draw.SetAtmosphereEnabled(true);
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
void SceneTransparency::AppendRemote(Graph& primary, std::vector<ID3D12Resource*>& primaryBindings,
    SceneGeometry& geometry, UINT slot, Frame& frame, GDX12Texture* hdr, GDX12Texture* sun, GDX12Texture* local,
    SceneEnvironment& environment, SceneReflections& reflections)
{
	auto& r = *_remote;
	auto& submit = *r.GPU.Submissions();
	auto& plan = r.Plan;
	auto& work = r.Work;
	auto& bindings = r.Bindings;
	auto& scene = geometry._slots.at(slot);
	auto& constants = *_resources.FrameConstants.at(slot);
	auto* depth = geometry.Depth(slot);
	const auto native = [](GDX12Texture* t){ return t->GetResource()->D3DResource.Get(); };
	const auto primaryId = [&](ID3D12Resource* value){ return NativeWork::ResourceFor(primary, primaryBindings, value, "WBOIT source"); };
	const auto id = [&](ID3D12Resource* value){ return NativeWork::ResourceFor(plan, bindings, value, "WBOIT owner input"); };
	plan = {};
	plan.Device = r.Owner;
	plan.FrameBoundary = false;
	bindings.clear();
	work.Commands.clear();
	r.Mirror.Begin();
	std::unordered_set<ID3D12Resource*> live;
	for (const auto& entry : _remoteSlots)
		if (entry && entry->SharedInputs == r.SharedInputs) live.insert(entry->Live.begin(), entry->Live.end());
	r.Mirror.RetireUnused(live, [](auto*){});
	r.Live.clear();
	r.Frame = _frame;
	r.Bootstrap = !r.SharedInputs->Scheduled;
	r.SharedInputs->Scheduled = true;
	r.Consumer = r.Applied = r.Imported = r.Produced = r.Resolved = r.MeshDone = {};
	r.TextureBytes = r.ShadowBytes = r.OwnerShadowBytes = 0;
	r.Shadows = _lighting->ShadowInputs(r.Owner);
	const auto mirror = [&](ID3D12Resource* src, bool refresh = true){
		r.Live.insert(src);
		return r.Mirror.Mirror(primary, primaryBindings, src, primaryId(src), refresh);
	};
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
	// Append any exact camera-LUT import before freezing its native resources.
	std::vector<Access> aerial;
	environment.AddAerialReads(primary, primaryBindings, aerial);
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
	mesh.Draws = scene.Draws;
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
	const auto cb = [&](ID3D12Resource* source, D3D12_GPU_VIRTUAL_ADDRESS address){
		auto* target = mirror(source);
		addRead(target, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
		return target->GetGPUVirtualAddress() + (address - source->GetGPUVirtualAddress());
	};
	mesh.Main = cb(primaryBindings[scene.FrameIds[0]], scene.Main);
	mesh.Camera = cb(primaryBindings[scene.FrameIds[1]], scene.Camera);
	mesh.Shadow = cb(constants.ShadowCB->GetResource().D3DResource.Get(), constants.ShadowCB->GetElementAddress(0));
	mesh.Sky = cb(constants.SkyCB->GetResource().D3DResource.Get(), constants.SkyCB->GetElementAddress(0));
	const auto structured = [&](UINT param, ID3D12Resource* src, UINT elements, UINT stride){
		auto* target = mirror(src);
		D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
		desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
		desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		desc.Buffer.NumElements = elements;
		desc.Buffer.StructureByteStride = stride;
		mesh.Views[param] = view(r.SceneViews[param], target, desc);
		addRead(target);
	};
	structured(0, scene.Buffers[2].Data.Get(), UINT(scene.Buffers[2].Bytes / sizeof(GDX12MaterialConstants)), sizeof(GDX12MaterialConstants));
	structured(1, scene.Buffers[3].Data.Get(), UINT(scene.Buffers[3].Bytes / sizeof(GDX12TransformConstants)), sizeof(GDX12TransformConstants));
	structured(2, scene.Buffers[0].Data.Get(), UINT(scene.Buffers[0].Bytes / sizeof(GDX12InstanceData)), sizeof(GDX12InstanceData));
	structured(7, constants.ShadowLights->GetResource().D3DResource.Get(), constants.ShadowLights->GetElementCount(), sizeof(GDX12LocalLightConstants));
	structured(10, constants.ReflectionProbes->GetResource().D3DResource.Get(), constants.ReflectionProbes->GetElementCount(), sizeof(GDX12ReflectionProbeData));
	std::vector<Id> shadowExports = r.Shadows.PrimaryDependencies;
	shadowExports.push_back(r.DepthExport);
	const auto shadow = [&](UINT param, GDX12Texture* atlas, const auto& copies, WboitShadowInput& stage, bool csm){
		auto* src = native(atlas);
		ID3D12Resource* target;
		if (copies.empty())
		{
			target = mirror(src);
			r.ShadowBytes += r.Mirror.PendingTransferBytes(src);
		}
		else
		{
			target = stage.Prepare(submit, r.Owner, primary, primaryBindings, _work, plan, bindings, work, src, copies, csm);
			if (stage.Export != Invalid) shadowExports.push_back(stage.Export);
			r.ShadowBytes += stage.Bytes;
			r.OwnerShadowBytes += stage.LocalBytes;
		}
		mesh.Views[param] = view(r.SceneViews[param], target, atlas->ShaderViewDescription());
		addRead(target);
	};
	shadow(3, sun, r.Shadows.Sun, r.Sun, true);
	shadow(8, local, r.Shadows.Local, r.Local, false);
	for (UINT i = 0; i < 3; ++i)
	{
		auto* source = environment.Output(i)->GetTexture();
		auto* target = mirror(native(source), environment.UpdatesTexture(*source));
		mesh.Views[i+4] = view(r.SceneViews[i+4], target, source->ShaderViewDescription());
		addRead(target);
	}
	const std::array skyNulls{r.Resources.SkyTransmittanceSRV.GPUHandle, r.Resources.SkyAerialRadianceSRV.GPUHandle,
		r.Resources.SkyAerialTransmittanceSRV.GPUHandle};
	for (UINT i = 0; i < mesh.Atmosphere.size(); ++i)
	{
		mesh.Atmosphere[i] = skyNulls[i];
		if (auto* lut = environment.TransparencyLut(i))
		{
			auto* target = mirror(native(lut), i != 0 || environment.UpdatesTexture(*lut));
			mesh.Atmosphere[i] = view(r.SkyViews[i], target, lut->ShaderViewDescription());
			addRead(target);
		}
	}
	auto* probeSource = native(reflections.Published());
	const Id probeId = primaryId(probeSource);
	const bool published = std::any_of(primary.Operations.begin(), primary.Operations.end(), [&](const auto& op){
		return std::any_of(op.Accesses.begin(), op.Accesses.end(), [=](auto access){
			return access.Resource == probeId && access.Usage != Mode::Read;
		});
	});
	auto* probes = mirror(probeSource, published);
	D3D12_SHADER_RESOURCE_VIEW_DESC probeView{};
	probeView.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	probeView.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
	probeView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	probeView.TextureCubeArray.NumCubes = probes->GetDesc().DepthOrArraySize / 6;
	probeView.TextureCubeArray.MipLevels = probes->GetDesc().MipLevels;
	mesh.Views[9] = view(r.SceneViews[9], probes, probeView);
	addRead(probes);

	auto* vertices = scene.Geometry.VertexResource;
	const auto meshRevision = _resources.GeometryBuffer->GetMeshCacheRevision();
	const bool newGeometry = r.SharedInputs->MeshRevision != meshRevision || !r.Mirror.Contains(vertices);
	std::vector<ProbeInputMirror::BufferRange> ranges;
	if (!newGeometry)
		for (auto [first, end] : geometry.ChangedVertexRanges(r.SharedInputs->VertexRevision))
			ranges.push_back({UINT64(first) * sizeof(Vertex), UINT64(end-first) * sizeof(Vertex)});
	std::vector<ProbeInputMirror::BufferPatch> patches;
	const auto skin = scene.RemoteSkin.find(r.Owner);
	r.Skin.reset();
	if ((newGeometry || !ranges.empty()) && skin != scene.RemoteSkin.end())
	{
		r.Skin = skin->second.Output;
		for (const auto& span : skin->second.Ranges)
			patches.push_back({r.Skin->Get(), UINT64(span.FirstVertex) * sizeof(Vertex),
				UINT64(span.Work.OutputVertex) * sizeof(Vertex), span.Work.VertexBytes()});
	}
	r.Live.insert(vertices);
	mesh.Geometry = scene.Geometry;
	mesh.Geometry.VertexResource = r.Mirror.Mirror(primary, primaryBindings, vertices, primaryId(vertices), newGeometry,
		std::move(patches), std::move(ranges));
	mesh.Geometry.IndexResource = mirror(scene.Geometry.IndexResource, newGeometry);
	mesh.Geometry.Vertices.BufferLocation = mesh.Geometry.VertexResource->GetGPUVirtualAddress();
	mesh.Geometry.Indices.BufferLocation = mesh.Geometry.IndexResource->GetGPUVirtualAddress();
	r.GeometryBytes = r.Mirror.PendingTransferBytes(vertices) + r.Mirror.PendingTransferBytes(scene.Geometry.IndexResource);
	r.SharedInputs->MeshRevision = meshRevision;
	r.SharedInputs->VertexRevision = geometry._vertexDeformationRevision;
	addRead(mesh.Geometry.VertexResource, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	addRead(mesh.Geometry.IndexResource, D3D12_RESOURCE_STATE_INDEX_BUFFER);
	mesh.Args = mirror(scene.Buffers[6].Data.Get());
	mesh.Count = mirror(scene.Buffers[7].Data.Get());
	addRead(mesh.Args, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
	addRead(mesh.Count, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);

	std::unordered_set<UINT> indices;
	const auto& materials = *constants.MaterialCache;
	for (UINT i = 0; i < constants.MaterialCache->GetElementCount(); ++i)
	{
		const auto& m = materials.GetCPUData()[i];
		if (m.RenderLayer != UINT(MaterialType::Transparent) || m.ReflectionProbePreview) continue;
		indices.insert({m.DiffuseIndex, m.NormalIndex, m.DisplacementIndex, m.SpecularIndex, m.RoughnessIndex, m.EmissiveIndex});
	}
	std::unordered_set<Id> restore;
	const bool refreshAssets = r.SharedInputs->TextureRevision != _resources.TextureRevision;
	const auto asset = [&](GDX12Texture& texture){
		auto* src = native(&texture);
		auto found = std::find(primaryBindings.begin(), primaryBindings.end(), src);
		if (found == primaryBindings.end())
		{
			const auto published = texture.GetResource()->GetCurrentState();
			const Id index = primaryId(src);
			primary.Resources[index].Initial = primary.Resources[index].Final = published;
		}
		auto* target = mirror(src, refreshAssets);
		auto desc = texture.ShaderViewDescription();
		r.Resources.Device->GetDevice()->CreateShaderResourceView(target, &desc, heap->GetCPUHandle(texture.GetSRV()->HeapIndex));
		if (r.Mirror.PendingCopy(src) && restore.insert(primaryId(src)).second) r.TextureBytes += r.Mirror.PendingTransferBytes(src);
		return target;
	};
	for (const auto& [_, texture] : _resources.Textures)
	{
		const UINT index = texture->GetSRV()->HeapIndex;
		if (index >= Texture2D_StartIndex && indices.count(index - Texture2D_StartIndex)) addRead(asset(*texture));
	}
	r.SharedInputs->TextureRevision = _resources.TextureRevision;
	r.Mesh = work.Add(plan, "WBOIT owner clear and draw transparent mesh", std::move(accesses), mesh);
	const Id cameraId = id(mirror(primaryBindings[scene.FrameIds[1]]));
	_particles.AppendRemoteWeighted(primary, primaryBindings, plan, bindings, work, r.Resources, accum, reveal, &r.Depth,
	    cameraId, mesh.Camera, [&](ID3D12Resource* src){return mirror(src);}, r.Particles, asset);

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
	r.Mirror.TrimBootstrapStaging();
	const Id imported = r.Mirror.AppendImport(plan, bindings, work, true);
	if (imported != 0) throw std::logic_error("WBOIT inputs must precede all owner work");
	++r.OwnerResolve;
	++r.Mesh;
	// Shadow assembly may have only owner-local inputs. Its version is still gated by GPU0's export fence.
	for (Id i = 1; i < plan.Operations.size(); ++i) plan.Operations[i].Dependencies.push_back(0);
	r.Export = r.Mirror.AppendExport(primary, primaryBindings, _work, Queue::Direct);
	auto& dependencies = primary.Operations.at(r.Export).Dependencies;
	dependencies.insert(dependencies.end(), shadowExports.begin(), shadowExports.end());
	if (!restore.empty())
	{
		std::vector<Access> reads;
		for (Id src : restore) reads.push_back({src, Mode::Read, read});
		_work.Add(primary, "restore GPU0 bindless textures after transparency export", std::move(reads), [](auto*){}, {r.Export});
	}
	std::vector<ID3D12Resource*> result{native(output)};
	auto* motion = r.Particles.Pass.MotionOutput();
	if (motion)
	{
		result.push_back(native(motion));
	}
	if (!r.Transfer || resized || r.Motion != bool(motion))
		r.Transfer = std::make_unique<WboitResultTransfer>(submit, r.Owner, result);
	r.Motion = bool(motion);
	r.Inputs = r.Mirror.TransferBytes() + r.DepthBytes;
	for (const auto* stage : {&r.Sun, &r.Local})
		if ((stage == &r.Sun && !r.Shadows.Sun.empty()) || (stage == &r.Local && !r.Shadows.Local.empty())) r.Inputs += stage->Bytes;
	r.Transfer->Append(plan, bindings, work, result, true);
	if (r.SharedInputs->ReportedFirst && r.SharedInputs->ReportedRegular) return;
	r.EmitterCounts.fill(0);
	if (!r.Particles.Reads.empty())
		for (const auto& source : r.Particles.Frame.Sources)
		{
			if (source.Emitter.Blend == ParticleBlend::WeightedAlpha) ++r.EmitterCounts[0];
			if (source.Emitter.Blend == ParticleBlend::SortedAlpha) ++r.EmitterCounts[1];
			if (source.Emitter.Blend == ParticleBlend::Additive) ++r.EmitterCounts[2];
		}
	r.TransparentDraws = 0;
	auto* commands = _resources.IndirectCommandsCache->GetCPUData();
	auto* instances = constants.InstanceCache->GetCPUData();
	for (UINT i = 0; i < _resources.IndirectCommandsCache->GetElementCount(); ++i)
		if (commands[i].InstanceCount && materials.GetCPUData()[instances[commands[i].InstanceID].MaterialIndex].RenderLayer == UINT(MaterialType::Transparent))
			++r.TransparentDraws;
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
	std::vector<ID3D12Resource*> destination{native(&frame.ImportedLayer)};
	if (r.Motion) destination.push_back(native(_particles.MotionOutput()));
	r.Import = r.Transfer->Append(primary, primaryBindings, _work, destination, false);
	primary.Operations[r.Import].Dependencies.push_back(r.Export);
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

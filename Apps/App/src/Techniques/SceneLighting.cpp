#include "SceneLighting.h"
#include "ProbeInputMirror.h"
#include "ShadowDepthTransfer.h"
#include "ShadowGeometry.h"
#include "SunShadowTileTransfer.h"
#include "Common/ConsoleVariables.h"
#include "Engine.Core/Logging/Logger.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include <FidelityFX/host/ffx_sssr.h>
#include <FidelityFX/host/backends/dx12/ffx_dx12.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
namespace BistroRender
{
namespace
{
AutoConsoleVariable<bool> AsyncLightGrid(L"App.Async.LightGrid", false,
                                        L"Build the light grid on the local COMPUTE queue.",
                                        EConsoleVariableFlags::Default);
AutoConsoleVariable<int> SunShadowAdapter(L"App.Shadows.Sun.Adapter", -1,
    L"DXGI hardware index for far CSM cascades; -1 uses GPU0", EConsoleVariableFlags::Default,
    [](int index) { return index >= -1; });
AutoConsoleVariable<UINT> SunShadowNearCount(L"App.Shadows.Sun.NearCascades", 2,
    L"Number of nearest active sun cascades drawn on GPU0", EConsoleVariableFlags::Default,
    [](UINT count) { return count <= 4; });
AutoConsoleVariable<int> SunShadowSync(L"App.Shadows.Sun.CrossSync", 1,
    L"Remote sun shadow freshness: 1 Exact, 3 Latest", EConsoleVariableFlags::Default,
    [](int mode) { return mode == 1 || mode == 3; });

bool CompatibleLocalShadow(const GDX12LocalLightConstants& cached, const GDX12LocalLightConstants& current)
{
	return GetLocalShadowKey(cached) == GetLocalShadowKey(current) && cached.Type == current.Type &&
	       cached.ShadowRevision == current.ShadowRevision && cached.ShadowFirstSlice == current.ShadowFirstSlice &&
	       (cached._padding[2] & current._padding[2]) == current._padding[2] && cached.Range == current.Range &&
	       std::memcmp(&cached.Position, &current.Position, sizeof(current.Position)) == 0 &&
	       std::memcmp(cached.ShadowViewProj, current.ShadowViewProj, sizeof(current.ShadowViewProj)) == 0;
}

constexpr UINT LightGridThreads = 64;
constexpr UINT LightingTileSize = 8;
constexpr UINT MaxDispatchGroups = D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION;
FfxResource Import(GDX12Texture* texture, FfxResourceStates state = FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ)
{
	auto* resource = texture->GetResource()->D3DResource.Get();
	return ffxGetResourceDX12(resource, ffxGetResourceDescriptionDX12(resource), L"Bistro SSSR", state);
}
void MatrixBytes(float* target, const Matrix& value)
{
	std::memcpy(target, &value, sizeof(value));
}
std::vector<Access> UniqueLightingAccesses(std::vector<Access> accesses)
{
	std::vector<Access> result;
	for (auto a : accesses)
	{
		auto found = std::find_if(result.begin(), result.end(), [&](auto b) { return a.Resource == b.Resource; });
		if (found == result.end())
		{
			result.push_back(a);
		}
		else if (a.Usage != found->Usage || a.Required != found->Required)
		{
			throw std::logic_error("Conflicting lighting access");
		}
	}
	return result;
}
} // namespace

struct SceneLighting::RemoteShadows
{
	RemoteShadows(const std::shared_ptr<QueueSubmission>& submit, Id owner, RenderPipelineCommonData& common)
	    : GPU(submit, owner, 1), Mirror(*submit, owner), Vegetation(Resources)
	{
		Resources.Initialize(&submit->Device(owner));
		Local = std::make_unique<SceneLocalShadows>(Resources, common);
	}
	~RemoteShadows()
	{
		try { GPU.Drain(); } catch (...) {}
	}
	Executor GPU;
	ProbeInputMirror Mirror;
	GDX12DeviceResources Resources;
	SceneGeometry::RemoteProbeVegetation Vegetation;
	std::unique_ptr<SceneLocalShadows> Local;
	std::unique_ptr<SceneSunShadows> Sun;
	std::unique_ptr<SunShadowTileTransfer> SunTransfer;
	std::array<GDX12Descriptor, 4> SceneViews;
	std::unique_ptr<ShadowDepthTransfer> Transfer;
	Graph Plan;
	NativeWork Work;
	std::vector<ID3D12Resource*> Bindings;
	Id Export = Invalid, Import = Invalid, Input = Invalid;
	CompletionToken Produced, Imported, Consumer;
	std::vector<UINT> Lights;
	std::vector<UINT> PendingSlices;
	std::map<UINT, GDX12LocalLightConstants> PublishedLights, PendingLights;
	GDX12ResourceHandle PublishedLocalAtlas, PublishedSunAtlas, PendingLocalAtlas, PendingSunAtlas;
	GDX12ShadowConstants PublishedSun{}, PendingSun{};
	UINT PublishedSunMask = 0, PendingSunMask = 0;
	uint64_t PublishedLocalPlacement = 0, PublishedSunPlacement = 0;
	uint64_t PendingSceneRevision = 0, PendingGrassRevision = 0;
	UINT Count = 0;
	UINT SunMask = 0, NearMask = 0;
	uint64_t MeshRevision = UINT64_MAX, TextureRevision = UINT64_MAX;
	bool Reported = false, Pending = false, Async = false;
};

SceneLighting::SceneLighting(GDX12DeviceResources& r, RenderPipelineCommonData& c)
    : _resources(r), _common(c), _frames(r.FrameConstants.size())
{
	if (r.FrameConstants.size() > FFX_MAX_QUEUED_FRAMES)
	{
		throw std::logic_error("SSSR queued-frame contract exceeded");
	}
	_reflections = std::make_unique<SceneReflections>(r, c);
	_sunShadows = std::make_unique<SceneSunShadows>(r, c);
	{
		_localShadows = std::make_unique<SceneLocalShadows>(r, c);
		_probeStaticShadows = std::make_unique<SceneLocalShadows>(r, c, true);
		_probePreview = std::make_unique<SceneProbePreview>(r, c);
		_transparency = std::make_unique<SceneTransparency>(r, c);
		GDX12RootSignatureDesc root;
		root.Constants = {9};
		root.NumSingleCBVSlots = 2;
		root.NumSingleSRVSlots = 1;
		root.NumSingleUAVSlots = 1;
		_gridRoot = std::make_unique<GDX12RootSignature>(r.Device, root);
		D3D12_FEATURE_DATA_D3D12_OPTIONS1 options{};
		const bool waves = SUCCEEDED(r.Device->GetDevice()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options,
		                                                                        sizeof(options))) &&
		                   options.WaveOps;
		const D3D_SHADER_MACRO defines[] = {{"LIGHT_GRID_WAVE_OPS", waves ? "1" : "0"}, {nullptr, nullptr}};
		_gridBuild = GDX12RenderHelpers::CreateComputePipeline(r.Device, _gridRoot.get(),
		                                                       SHADERS_FOLDER "LightGrid.hlsl", "BuildCS", defines);
	}
	_deferred._opaqueOnly = true;
	_deferred.SetFlag(RENDER_PASS_FLAG_USE_DOWNSCALED_RESOLUTION, true);
}
SceneLighting::~SceneLighting()
{
	if (_ssr)
	{
		_ssr->ClearDenendencies();
	}
	if (_initialized)
	{
		_deferred.ClearDenendencies();
	}
}
void SceneLighting::Reset()
{
	_pendingReset = true;
}
void SceneLighting::Freeze(SceneGeometry& geometry, UINT slot, const std::shared_ptr<QueueSubmission>& submissions, Id aoDevice)
{
	_submissions = submissions;
	if (_pendingReset)
	{
		_reflections->Reset();
		_forceReset = true;
		_pendingReset = false;
	}
	_reflections->Freeze(submissions);
	_ssrSettings = GetSSRSettings();
	_gridSettings = GetLightGridSettings();
	_slot = slot;
	auto& scene = geometry._slots.at(slot);
	auto& frame = _frames.at(slot);
	frame.Width = scene.DepthView->GetWidth();
	frame.Height = scene.DepthView->GetHeight();
	auto* normals = scene.Color[2].get();
	if (!frame.AO)
	{
		frame.AO = std::make_unique<SceneAO>(_resources, _common, scene.DepthView.get(), normals);
	}
	frame.AO->Freeze(scene.DepthView.get(), normals, scene.Color[1].get(), geometry.PrepassNormals(slot), _aoTemporalIndex,
	                 submissions, aoDevice, _aoHistory);

	auto& constants = *_resources.FrameConstants.at(slot);
	auto vegetation = geometry.VegetationPrimitives();
	// The static cache reads the change journals before the main cache consumes them.
	auto staticVegetation = vegetation;
	staticVegetation.erase(std::remove_if(staticVegetation.begin(), staticVegetation.end(),
	                                      [](const auto& primitive) { return primitive.Dynamic; }), staticVegetation.end());
	_probeStaticShadows->Freeze(slot, staticVegetation, true);
	auto& placements = _localShadows->_placements;
	std::vector<std::pair<GDX12LocalShadowKey, Id>> resolved;
	for (const auto& placement : _resources.LocalShadowPlacements)
		if (const Id owner = submissions->SelectAdapter(placement.Adapter))
			resolved.emplace_back(placement.Key, owner);
	if (placements != resolved)
	{
		placements = std::move(resolved);
		++_localPlacementEpoch;
	}
	_localShadows->Freeze(slot, vegetation, true);
	_sunShadows->Freeze(slot, _localShadows->CastersRebuilt(), _localShadows->ChangedCasterBounds(), vegetation);
	auto& sun = _sunShadows->_slots.at(slot);
	const Id sunOwner = submissions->SelectAdapter(SunShadowAdapter.GetValue());
	const UINT cascadeCount = _resources.ShadowSettings.CascadeCount;
	const UINT nearCount = sunOwner ? (std::min)(cascadeCount, SunShadowNearCount.GetValue()) : cascadeCount;
	const UINT active = (1u << cascadeCount) - 1;
	const UINT sunMask = active & ~((1u << nearCount) - 1);
	const int sync = SunShadowSync.GetValue();
	if (_sunOwner != sunOwner || _sunRemoteMask != sunMask || _sunSync != sync)
		++_sunPlacementEpoch;
	_sunOwner = sunOwner;
	_sunRemoteMask = sunMask;
	_sunSync = sync;
	if (sun.Owner != _sunOwner || sun.Near != nearCount)
	{
		if (_resources.Lighting.SunShadowFlags & ShadowEnabled) sun.Update |= active;
		sun.DirtyCascades |= active;
		sun.Owner = _sunOwner;
		sun.Near = nearCount;
	}
	constants.ShadowCB->CopyData(0, _resources.ShadowSettings);
	if (!constants.LocalLightCache->GetElementCount())
	{
		constants.LocalLightCache->Resize(1);
		constants.LocalLightCache->CopyData(0, GDX12LocalLightConstants{});
	}
	_probeStaticShadows->Complete(slot);
	auto& staticLights = _probeStaticShadows->LightSource(slot);
	if (!staticLights.GetSRV())
		staticLights.CreateSRV(_resources.SRV_UAV_Heap.get(), _resources.SRV_UAV_Heap->GetAvailableIndex(SceneDescriptorStart));
	_localShadows->Complete(slot);
	auto& lightSource = _localShadows->LightSource(slot);
	const UINT count = lightSource.GetElementCount();
	if (!constants.ShadowLights)
	{
		constants.ShadowLights = std::make_unique<GDX12UploadBuffer<GDX12LocalLightConstants>>(
		    _resources.Device, count, EBufferType::Default, false);
		constants.ShadowLights->CreateSRV(_resources.SRV_UAV_Heap.get(),
		                                  _resources.SRV_UAV_Heap->GetAvailableIndex(SceneDescriptorStart));
	}
	else
	{
		constants.ShadowLights->Resize(count);
	}
	{
		auto& source = lightSource;
		frame.UploadLights = frame.LightRevision != source.GetRevision() ||
		                     frame.LightSource.Get() != source.GetResource().D3DResource.Get();
		if (frame.UploadLights)
		{
			frame.LightRevision = source.GetRevision();
			frame.LightSource = source.GetResource().D3DResource;
		}
	}
}

void SceneLighting::AppendGpuLightGrid(Graph& graph, std::vector<ID3D12Resource*>& bindings,
                                       const SceneGeometry& geometry)
{
	auto& scene = geometry._slots.at(_slot);
	const auto& frame = _frames.at(_slot);
	auto& grid = _deferred._grids.at(_slot);
	const auto& layout = frame.GridLayout;
	const auto resource = [&](GDX12Resource& value)
	{ return NativeWork::ResourceFor(graph, bindings, value.D3DResource.Get(), "GPU light grid resource"); };
	const Id masks = resource(grid.Masks->GetResource());
	const Id constants = resource(grid.Constants->GetResource());
	auto& lights = *_resources.FrameConstants.at(_slot)->ShadowLights;
	const Id sources = resource(lights.GetResource());
	if (frame.UploadLights)
	{
		auto& source = _localShadows->LightSource(_slot);
		auto* from = source.GetResource().D3DResource.Get();
		auto* to = lights.GetResource().D3DResource.Get();
		const UINT64 bytes = UINT64(source.GetElementCount()) * sizeof(GDX12LocalLightConstants);
		_work.Add(graph, "local-light snapshot upload",
		          {{resource(source.GetResource()), Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ},
		           {sources, Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST}},
		          [=](auto* cmd) { cmd->CopyBufferRegion(to, 0, from, 0, bytes); });
	}
	const UINT groups = layout.Size.w ? ((layout.Size.x * layout.Size.y + LightGridThreads - 1) / LightGridThreads) *
	                                        layout.Size.z * layout.Size.w
	                                  : 1;
	std::array<UINT, 9> parameters{frame.Width, frame.Height, UINT(_resources.LocalLights.size()), groups};
	const float projection[] = {_common.CameraProjection._11, _common.CameraProjection._22,
	                            _common.CameraProjection._31, _common.CameraProjection._32,
	                            _common.ActiveCameraFarPlane};
	std::memcpy(parameters.data() + 4, projection, sizeof(projection));
	auto* root = _gridRoot.get();
	auto* heap = _resources.SRV_UAV_Heap->GetHeap().Get();
	auto* build = _gridBuild.Get();
	const auto camera = scene.Camera;
	const auto gridCB = grid.Constants->GetElementAddress(0);
	const auto srv = lights.GetSRV()->GPUHandle;
	const auto uav = grid.Masks->GetUAV()->GPUHandle;
	const bool async = AsyncLightGrid.GetValue();
	const State lightRead = async ? State(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
	                              : State(D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
	_work.Add(graph, "GPU light grid masks",
	          {{masks, Mode::Write, UnorderedAccess},
	           {sources, Mode::Read, lightRead},
	           {scene.FrameIds[1], Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ},
	           {constants, Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ}},
	          [=](auto* cmd)
	          {
		          cmd->SetDescriptorHeaps(1, &heap);
		          cmd->SetComputeRootSignature(root->GetRootSignature().Get());
		          cmd->SetPipelineState(build);
		          cmd->SetComputeRoot32BitConstants(0, UINT(parameters.size()), parameters.data(), 0);
		          cmd->SetComputeRootConstantBufferView(root->GetBRootParamIndex(1), camera);
		          cmd->SetComputeRootConstantBufferView(root->GetBRootParamIndex(2), gridCB);
		          cmd->SetComputeRootDescriptorTable(root->GetTRootParamIndex(0), srv);
		          cmd->SetComputeRootDescriptorTable(root->GetURootParamIndex(0), uav);
		          cmd->Dispatch((std::min)(groups, MaxDispatchGroups),
		                        (groups + MaxDispatchGroups - 1) / MaxDispatchGroups, 1);
	          }, {}, async ? Queue::Compute : Queue::Direct);
}
void SceneLighting::AppendAO(Graph& graph, std::vector<ID3D12Resource*>& bindings)
{
	_frames.at(_slot).AO->Append(graph, bindings);
}
bool SceneLighting::Record(Id id, ID3D12GraphicsCommandList* commands) const
{
	return _work.Record(id, commands) || (_sunShadows->Record(_slot, id, commands)) ||
	       (_localShadows->Record(_slot, id, commands)) || (_probeStaticShadows->Record(_slot, id, commands)) ||
	       (_probePreview->Record(id, commands)) || (_transparency->Record(id, commands)) ||
	       (_reflections->Record(id, commands)) || (_frames.at(_slot).AO && _frames.at(_slot).AO->Record(id, commands));
}
void SceneLighting::Submitted(uint64_t fence)
{
	_probeStaticShadows->Submitted(_slot);
	_localShadows->Submitted(_slot);
	_reflections->Submitted(fence);
}
void SceneLighting::Append(Graph& graph, std::vector<ID3D12Resource*>& bindings, SceneGeometry& geometry,
                           SceneEnvironment& environment)
{
	_work.Commands.clear();
	_frames.at(_slot).AO->AppendRemoteDepth(graph, bindings);
	AppendShadows(graph, bindings, geometry, environment);
	PrepareDeferred(geometry, environment);
	AppendAO(graph, bindings);
	AppendDeferred(graph, bindings, geometry, environment);
	AppendComposition(graph, bindings, geometry, environment, AppendSSR(graph, bindings, geometry, environment));
}
void SceneLighting::AppendShadows(Graph& graph, std::vector<ID3D12Resource*>& bindings, SceneGeometry& geometry,
                                  SceneEnvironment& environment)
{
	auto& scene = geometry._slots.at(_slot);
	auto& constants = *_resources.FrameConstants.at(_slot);
	const auto appendGrass = [&](const GeometryTarget& view, Graph& g, std::vector<ID3D12Resource*>& resources)
	{ geometry.AppendVegetationView(g, resources, _slot, view); };
	_sunShadows->Append(graph, bindings, _slot,
	                    {scene.Buffers[0].Srv.GPUHandle, scene.Buffers[1].Srv.GPUHandle, scene.Buffers[2].Srv.GPUHandle,
	                     scene.Buffers[3].Srv.GPUHandle},
	                    scene.Draws, scene.Geometry, appendGrass, ~_sunRemoteMask);
	AppendRemoteLocalShadows(graph, bindings, geometry);
	_localShadows->Append(graph, bindings, _slot,
	                      {scene.Buffers[0].Srv.GPUHandle, scene.Buffers[1].Srv.GPUHandle,
	                       scene.Buffers[2].Srv.GPUHandle, scene.Buffers[3].Srv.GPUHandle},
	                      scene.Geometry, *constants.ShadowLights, appendGrass);
	{
		_probeStaticShadows->Append(graph, bindings, _slot,
		                            {scene.Buffers[0].Srv.GPUHandle, scene.Buffers[1].Srv.GPUHandle,
		                             scene.Buffers[2].Srv.GPUHandle, scene.Buffers[3].Srv.GPUHandle},
		                            scene.Geometry, _probeStaticShadows->LightSource(_slot), appendGrass);
		_reflections->Append(graph, bindings, geometry, environment, _slot, _localShadows->Output(_slot),
		                     _probeStaticShadows->Output(_slot), _probeStaticShadows->LightSource(_slot));
	}
}

void SceneLighting::AppendRemoteLocalShadows(Graph& primary, std::vector<ID3D12Resource*>& primaryBindings,
                                            SceneGeometry& geometry)
{
	auto& source = _localShadows->_slots.at(_slot);
	auto& owners = _frames.at(_slot).ShadowOwners;
	auto& sun = _sunShadows->_slots.at(_slot);
	const auto localAtlas = source.Atlas.GetResource()->D3DResource;
	const auto sunAtlas = sun.Atlas.GetResource()->D3DResource;
	const auto sunSettings = _resources.ShadowSettings;
	const auto desiredLatest = [&](const GDX12LocalLightConstants& light)
	{
		return std::any_of(_resources.LocalShadowPlacements.begin(), _resources.LocalShadowPlacements.end(),
		    [&](const auto& item) { return item.Key == GetLocalShadowKey(light) && item.CrossSync == ShadowCrossSync::Latest; });
	};
	const auto localCompatible = [&](const RemoteShadows& remote, const GDX12LocalLightConstants& light)
	{
		const auto it = remote.PublishedLights.find(light.EntityId);
		return remote.PublishedLocalPlacement == _localPlacementEpoch &&
		       remote.PublishedLocalAtlas.Get() == localAtlas.Get() && it != remote.PublishedLights.end() &&
		       CompatibleLocalShadow(it->second, light);
	};
	const auto sunCompatible = [&](const RemoteShadows& remote)
	{
		return remote.PublishedSunPlacement == _sunPlacementEpoch &&
		       remote.PublishedSunAtlas.Get() == sunAtlas.Get() && remote.PublishedSunMask == _sunRemoteMask &&
		       std::memcmp(&remote.PublishedSun, &sunSettings, sizeof(sunSettings)) == 0;
	};
	std::set<std::pair<Id, bool>> importing;
	for (auto& [lane, remote] : owners)
	{
		const Id owner = lane.first;
		remote->Export = remote->Import = Invalid;
		if (!remote->Reported && remote->Consumer && _submissions->Complete(remote->Consumer))
		{
			_submissions->LogFirstImport("local shadow depth", remote->Produced, remote->Imported);
			for (const UINT light : remote->Lights)
				Log::Info("RenderGraph", "GPU0 completed local shadow consumer fence=", remote->Consumer.Value,
				          " entity=", light, " slices=", remote->Count, " owner=", _submissions->AdapterLabel(owner));
			if (remote->SunMask)
				Log::Info("RenderGraph", "GPU0 completed CSM consumer fence=", remote->Consumer.Value,
				          " GPU0 cascade mask=", remote->NearMask, " imported cascade mask=", remote->SunMask,
				          " owner=", _submissions->AdapterLabel(owner));
			remote->Reported = true;
		}
		if (!remote->Pending || !_submissions->Complete(remote->Produced)) continue;
		remote->Pending = false;
		const bool localReady = !remote->PendingSlices.empty() && remote->PendingLocalAtlas.Get() == localAtlas.Get() &&
		    std::all_of(remote->PendingLights.begin(), remote->PendingLights.end(), [&](const auto& entry)
		    {
			    const auto current = std::lower_bound(source.Lights.begin(), source.Lights.end(), entry.first,
			        [](const auto& light, UINT entity) { return light.EntityId < entity; });
			    return current != source.Lights.end() && current->EntityId == entry.first &&
			        source.Cache[current - source.Lights.begin()].Owner == owner && desiredLatest(*current) &&
			        localCompatible(*remote, *current) && CompatibleLocalShadow(entry.second, *current);
		    });
		const bool sunReady = remote->PendingSunMask && owner == _sunOwner && SunShadowSync.GetValue() == 3 &&
		    remote->PendingSunAtlas.Get() == sunAtlas.Get() && sunCompatible(*remote) &&
		    std::memcmp(&remote->PendingSun, &sunSettings, sizeof(sunSettings)) == 0;
		if (localReady)
		{
			if (remote->PendingSceneRevision == source.SceneRevision && remote->PendingGrassRevision == source.GrassReceiverRevision)
				for (const auto& [entity, light] : remote->PendingLights)
				{
					const auto current = std::lower_bound(source.Lights.begin(), source.Lights.end(), entity,
					    [](const auto& item, UINT key) { return item.EntityId < key; });
					for (const UINT slice : remote->PendingSlices)
						if (slice >= light.ShadowFirstSlice && slice - light.ShadowFirstSlice < LocalShadowFaceCount(light))
							source.RemoteViews.push_back({UINT(current - source.Lights.begin()), slice - light.ShadowFirstSlice, slice});
				}
			remote->Import = remote->Transfer->Append(primary, primaryBindings, _work, localAtlas.Get(),
			                                       std::move(remote->PendingSlices), false);
		}
		if (sunReady)
		{
			const Id imported = remote->SunTransfer->Import(primary, primaryBindings, _work, sun.Atlas);
			if (remote->Import == Invalid) remote->Import = imported;
			else primary.Operations[imported].Dependencies.push_back(remote->Import);
			if (remote->PendingSceneRevision == source.SceneRevision && remote->PendingGrassRevision == source.GrassReceiverRevision)
				sun.DirtyCascades &= ~remote->PendingSunMask;
		}
		if (localReady || sunReady) importing.insert(lane);
	}
	std::map<std::pair<Id, bool>, std::vector<SceneLocalShadows::View>> views;
	source.Views.erase(std::remove_if(source.Views.begin(), source.Views.end(), [&](const auto& view)
	{
		const Id owner = source.Cache[view.Light].Owner;
		if (!owner) return false;
		const auto& light = source.Lights[view.Light];
		bool latest = desiredLatest(light);
		const auto old = owners.find({owner, true});
		if (latest && old != owners.end() && (old->second->Pending || importing.count({owner, true})))
		{
			if (localCompatible(*old->second, light)) return true;
			latest = false;
		}
		views[{owner, latest}].push_back(view);
		return true;
	}), source.Views.end());
	const UINT sunUpdate = sun.Update & _sunRemoteMask;
	std::pair<Id, bool> sunLane{_sunOwner, SunShadowSync.GetValue() == 3};
	bool orderSun = sunUpdate != 0;
	const auto oldSun = owners.find(sunLane);
	if (orderSun && sunLane.second && oldSun != owners.end() &&
	    (oldSun->second->Pending || importing.count(sunLane)))
	{
		if (sunCompatible(*oldSun->second)) orderSun = false;
		else sunLane.second = false;
	}
	if (orderSun) views.try_emplace(sunLane);
	if (views.empty()) return;

	auto& scene = geometry._slots.at(_slot);
	auto live = geometry.LiveProbeInputs();
	auto& materials = *_resources.FrameConstants.at(_slot)->MaterialCache;
	std::set<UINT> maskedTextures;
	for (UINT i = 0; i < materials.GetElementCount(); ++i)
	{
		const auto& material = materials.GetCPUData()[i];
		if (material.RenderLayer == UINT(MaterialType::Masked) && !material.ReflectionProbePreview)
			maskedTextures.insert(material.DiffuseIndex);
	}
	std::vector<GDX12Texture*> textures;
	for (const auto& [name, texture] : _resources.Textures)
	{
		const UINT index = texture->GetSRV()->HeapIndex;
		if (index < Texture2D_StartIndex || !maskedTextures.count(index - Texture2D_StartIndex)) continue;
		textures.push_back(texture.get());
		live.insert(texture->GetResource()->D3DResource.Get());
	}
	auto& camera = *_resources.FrameConstants.at(_slot)->CameraCB;
	auto* cameraBuffer = camera.GetResource().D3DResource.Get();
	live.insert(cameraBuffer);
	for (const auto& [lane, batch] : views)
	{
		const Id owner = lane.first;
		auto& entry = owners[lane];
		if (!entry) entry = std::make_unique<RemoteShadows>(_submissions, owner, _common);
		auto& remote = *entry;
		remote.SunMask = orderSun && lane == sunLane ? sunUpdate : 0;
		remote.NearMask = sun.Update & ~_sunRemoteMask;
		const bool latest = lane.second && (!remote.SunMask || sunCompatible(remote)) &&
		    std::all_of(batch.begin(), batch.end(), [&](const auto& view) { return localCompatible(remote, source.Lights[view.Light]); });
		remote.Async = latest;
		remote.PendingSceneRevision = source.SceneRevision;
		remote.PendingGrassRevision = source.GrassReceiverRevision;
		remote.PendingSlices.clear();
		remote.PendingLights.clear();
		remote.PendingSunMask = 0;
		auto& pass = *remote.Local;
		auto& target = pass._slots.front();
		remote.Resources.ShadowQuality.LocalResolution = source.Resolution;
		pass.PrepareAtlas(target, UINT(batch.size()));
		target.Lights = source.Lights;
		target.Views.clear();
		target.Packed.clear();
		std::vector<UINT> from, to;
		remote.Lights.clear();
		for (auto view : batch)
		{
			to.push_back(view.Slice);
			view.Slice = UINT(from.size());
			from.push_back(view.Slice);
			const auto first = source.Packed.begin() + view.First;
			view.First = UINT(target.Packed.size());
			target.Packed.insert(target.Packed.end(), first, first + view.OpaqueCount + view.MaskedCount);
			target.Views.push_back(view);
			const auto entity = target.Lights[view.Light].EntityId;
			if (remote.Lights.empty() || remote.Lights.back() != entity) remote.Lights.push_back(entity);
		}
		remote.Count = UINT(batch.size());
		auto* heap = remote.Resources.SRV_UAV_Heap.get();
		if (!batch.empty())
		{
			pass.Complete(0);
			if (!target.LightUpload->GetSRV()) target.LightUpload->CreateSRV(heap, heap->GetAvailableIndex(SceneDescriptorStart));
		}
		auto& graph = remote.Plan;
		auto& bindings = remote.Bindings;
		auto& work = remote.Work;
		graph = {};
		graph.Device = owner;
		graph.FrameBoundary = false;
		bindings.clear();
		work.Commands.clear();
		remote.Mirror.Begin();
		remote.Mirror.RetireUnused(live, [&](ID3D12Resource* retired) { remote.Vegetation.Forget(retired); });
		const auto mirror = [&](Id id, bool refresh)
		{ return remote.Mirror.Mirror(primary, primaryBindings, primaryBindings.at(id), id, refresh); };
		const auto bind = [&](ID3D12Resource* resource)
		{ return NativeWork::ResourceFor(graph, bindings, resource, "shadow owner input"); };
		std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 4> descriptors{};
		std::array<Id, 6> inputs{};
		constexpr UINT strides[]{sizeof(GDX12InstanceData), sizeof(GDX12IndirectDrawArgs),
		                         sizeof(GDX12MaterialConstants), sizeof(GDX12TransformConstants)};
		constexpr std::array sceneInputs{SceneInstances, SceneMaterials, SceneTransforms, SceneCommands};
		for (UINT output = 0; output < sceneInputs.size(); ++output)
		{
			const auto index = sceneInputs[output];
			auto* local = mirror(index, true);
			inputs[index == SceneCommands ? 5 : output] = bind(local);
			auto& descriptor = remote.SceneViews[index];
			D3D12_SHADER_RESOURCE_VIEW_DESC view{};
			view.Format = DXGI_FORMAT_UNKNOWN;
			view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
			view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			view.Buffer.StructureByteStride = strides[index];
			view.Buffer.NumElements = UINT(scene.Buffers[index].Bytes / strides[index]);
			descriptor.InitAsSRV(local, &view, heap, descriptor.HeapIndex == UINT_MAX
			    ? heap->GetAvailableIndex(SceneDescriptorStart) : descriptor.HeapIndex);
			descriptors[index] = descriptor.GPUHandle;
		}
		auto mesh = scene.Geometry;
		const auto meshRevision = _resources.GeometryBuffer->GetMeshCacheRevision();
		const bool meshChanged = remote.MeshRevision != meshRevision;
		remote.MeshRevision = meshRevision;
		mesh.VertexResource = mirror(SceneVertices, meshChanged || !scene.Skin.empty());
		mesh.IndexResource = mirror(SceneIndices, meshChanged);
		mesh.Vertices.BufferLocation = mesh.VertexResource->GetGPUVirtualAddress();
		mesh.Indices.BufferLocation = mesh.IndexResource->GetGPUVirtualAddress();
		inputs[3] = bind(mesh.VertexResource);
		inputs[4] = bind(mesh.IndexResource);
		const Id cameraSource = NativeWork::ResourceFor(primary, primaryBindings, cameraBuffer, "shadow camera input");
		auto* localCamera = mirror(cameraSource, true);
		const Id cameraId = bind(localCamera);
		const auto cameraAddress = localCamera->GetGPUVirtualAddress() +
		    UINT64(_common.ActiveCameraCBufferIndex) * camera.GetElementSize();
		const bool materialsChanged = remote.TextureRevision != _resources.TextureRevision;
		remote.TextureRevision = _resources.TextureRevision;
		std::vector<Access> materialReads, restore;
		for (auto* texture : textures)
		{
			auto& resource = *texture->GetResource();
			auto* native = resource.D3DResource.Get();
			const auto found = std::find(primaryBindings.begin(), primaryBindings.end(), native);
			Id id = Id(found - primaryBindings.begin());
			if (found == primaryBindings.end())
			{
				id = NativeWork::ResourceFor(primary, primaryBindings, native, "shadow material texture");
				primary.Resources[id].Initial = primary.Resources[id].Final = resource.GetCurrentState();
			}
			const bool refresh = materialsChanged || !remote.Mirror.Contains(native);
			auto* local = mirror(id, refresh);
			if (refresh)
			{
				const auto view = texture->ShaderViewDescription();
				remote.Resources.Device->GetDevice()->CreateShaderResourceView(
				    local, &view, heap->GetCPUHandle(texture->GetSRV()->HeapIndex));
			}
			constexpr State read = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
			materialReads.push_back({bind(local), Mode::Read, read});
			if (remote.Mirror.PendingCopy(native)) restore.push_back({id, Mode::Read, read});
		}
		materialReads = UniqueLightingAccesses(std::move(materialReads));
		const auto vegetation = [&](const GeometryTarget& view, Graph&, std::vector<ID3D12Resource*>&)
		{
			auto grass = view;
			grass.View.FrameSlot = _slot;
			grass.Camera = cameraId;
			grass.View.CameraConstants = cameraAddress;
			grass.AdditionalReads = materialReads;
			geometry.AppendVegetationView(primary, primaryBindings, remote.Mirror, graph, bindings, work,
			    remote.Vegetation, _slot, grass, remote.SceneViews[SceneMaterials], inputs[1], Invalid, 0, {});
		};
		const auto addWork = [&](NativeWork& planned)
		{
			work.Commands.insert(work.Commands.end(), std::make_move_iterator(planned.Commands.begin()),
			                     std::make_move_iterator(planned.Commands.end()));
		};
		if (!batch.empty())
		{
			const std::array<Id, 5> localInputs{inputs[0], inputs[1], inputs[2], inputs[3], inputs[4]};
			pass.Append(graph, bindings, 0, descriptors, mesh, *target.LightUpload, vegetation, &localInputs, materialReads);
			addWork(target.Work);
		}
		if (remote.SunMask)
		{
			if (!remote.Sun) remote.Sun = std::make_unique<SceneSunShadows>(remote.Resources, _common);
			auto& shadow = *remote.Sun;
			remote.Resources.ShadowQuality.SunResolution = _resources.ShadowQuality.SunResolution;
			remote.Resources.ShadowSettings = _resources.ShadowSettings;
			remote.Resources.FrameConstants.front()->ShadowCB->CopyData(0, _resources.ShadowSettings);
			shadow._slots.front().Update = remote.SunMask;
			shadow.Append(graph, bindings, 0, descriptors, scene.Draws, mesh, vegetation, remote.SunMask,
			              &inputs, materialReads);
			addWork(shadow._slots.front().Work);
		}
		std::sort(work.Commands.begin(), work.Commands.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
		remote.Input = remote.Mirror.AppendImport(graph, bindings, work, true);
		remote.Export = remote.Mirror.AppendExport(primary, primaryBindings, _work, restore.empty() ? Queue::Copy : Queue::Direct);
		Id restored = Invalid;
		if (!restore.empty())
			restored = _work.Add(primary, "restore GPU0 shadow material textures", UniqueLightingAccesses(std::move(restore)),
			                     [](auto*) {}, {remote.Export});
		scene.ShadowInputCopies.emplace_back(remote.Export, restored);
		if (!batch.empty())
		{
			remote.Transfer = std::make_unique<ShadowDepthTransfer>(*_submissions, owner, source.Resolution, UINT(batch.size()));
			remote.Transfer->Append(graph, bindings, work, target.Atlas.GetResource()->D3DResource.Get(), std::move(from), true);
			if (latest)
			{
				remote.PendingLocalAtlas = localAtlas;
				remote.PendingSlices = std::move(to);
				for (const auto& view : batch)
				{
					const auto& light = source.Lights[view.Light];
					remote.PendingLights[light.EntityId] = light;
				}
			}
			else
			{
				remote.Import = remote.Transfer->Append(primary, primaryBindings, _work, localAtlas.Get(), std::move(to), false);
				source.RemoteViews.insert(source.RemoteViews.end(), batch.begin(), batch.end());
				if (remote.PublishedLocalAtlas.Get() != localAtlas.Get() || remote.PublishedLocalPlacement != _localPlacementEpoch)
					remote.PublishedLights.clear();
				remote.PublishedLocalAtlas = localAtlas;
				remote.PublishedLocalPlacement = _localPlacementEpoch;
				const auto latestOwner = owners.find({owner, true});
				for (const auto& view : batch)
				{
					const auto& light = source.Lights[view.Light];
					if (lane.second) remote.PublishedLights[light.EntityId] = light;
					else if (latestOwner != owners.end()) latestOwner->second->PublishedLights.erase(light.EntityId);
				}
			}
		}
		if (remote.SunMask)
		{
			if (!remote.SunTransfer)
				remote.SunTransfer = std::make_unique<SunShadowTileTransfer>(*_submissions, owner,
				    *_resources.SRV_UAV_Heap, *heap);
			remote.SunTransfer->Prepare(_resources.ShadowQuality.SunResolution, remote.SunMask);
			remote.SunTransfer->Export(graph, bindings, work, *remote.Sun->Output(0));
			if (latest)
			{
				remote.PendingSunAtlas = sunAtlas;
				remote.PendingSunMask = remote.SunMask;
				remote.PendingSun = sunSettings;
			}
			else
			{
				const Id imported = remote.SunTransfer->Import(primary, primaryBindings, _work, sun.Atlas);
				if (remote.Import == Invalid) remote.Import = imported;
				else primary.Operations[imported].Dependencies.push_back(remote.Import);
				sun.Settings = sunSettings;
				sun.Revision = _resources.Lighting.SunShadowRevision;
				sun.DirtyCascades &= ~remote.SunMask;
				if (lane.second)
				{
					remote.PublishedSunAtlas = sunAtlas;
					remote.PublishedSunPlacement = _sunPlacementEpoch;
					remote.PublishedSunMask = _sunRemoteMask;
					remote.PublishedSun = sunSettings;
				}
				else if (const auto prior = owners.find({owner, true}); prior != owners.end())
					prior->second->PublishedSunMask = 0;
			}
		}
	}
}

void SceneLighting::AddShadowExchanges(const Schedule& primary, std::vector<Executor::PacketExchange>& exchanges)
{
	const auto packet = [](const Schedule& schedule, Id operation)
	{
		const auto it = std::find_if(schedule.Packets.begin(), schedule.Packets.end(),
		                             [=](const auto& entry) { return entry.Operation == operation; });
		if (it == schedule.Packets.end()) throw std::logic_error("Missing cross-adapter shadow packet");
		return Id(it - schedule.Packets.begin());
	};
	for (auto& [lane, entry] : _frames.at(_slot).ShadowOwners)
	{
		auto& remote = *entry;
		if (remote.Import != Invalid) remote.Import = packet(primary, remote.Import);
		if (remote.Export == Invalid) continue;
		auto schedule = Compile(remote.Plan);
		const Id input = packet(schedule, remote.Input);
		remote.GPU.Begin(0);
		remote.GPU.Reserve(schedule);
		exchanges.push_back({packet(primary, remote.Export), remote.Import,
		    [&remote, schedule = std::move(schedule), input](CompletionToken source)
		    {
			    remote.GPU.Execute(schedule, remote.Bindings, [&remote](Id op, UINT, ID3D12GraphicsCommandList* cmd)
			    {
				    if (!remote.Work.Record(op, cmd)) throw std::logic_error("Missing owner shadow work");
			    }, nullptr, nullptr, nullptr, {{input, source}});
			    remote.Produced = remote.GPU.Completions().back();
			    remote.Pending = remote.Async;
			    return remote.Produced;
		    }});
	}
}

void SceneLighting::ShadowImportsSubmitted(const std::vector<CompletionToken>& completions)
{
	for (auto& [owner, entry] : _frames.at(_slot).ShadowOwners)
	{
		auto& remote = *entry;
		if (remote.Import == Invalid) continue;
		remote.Imported = completions.at(remote.Import);
		remote.Consumer = completions.back();
	}
}

void SceneLighting::PrepareDeferred(SceneGeometry& geometry, SceneEnvironment& environment)
{
	auto& scene = geometry._slots.at(_slot);
	auto& frame = _frames.at(_slot);
	_deferred.Setup(&_resources, &_common);
	_deferred.SetInputs({scene.Color[0].get(), scene.Color[2].get(), scene.Color[3].get(), scene.Color[4].get(),
	                     scene.DepthView.get(), _sunShadows->Output(_slot), environment.Output(0),
	                     environment.Output(1), environment.Output(2), environment.Output(3),
	                     _localShadows->Output(_slot), frame.AO->Output()});
	if (!_initialized)
	{
		_deferred.Initialize();
		for (auto& grid : _deferred._grids)
		{
			grid.Masks->CreateUAV(_resources.SRV_UAV_Heap.get(),
			                      _resources.SRV_UAV_Heap->GetAvailableIndex(SceneDescriptorStart));
		}
	}
	else if (_deferred._color->GetWidth() != frame.Width || _deferred._color->GetHeight() != frame.Height)
	{
		// Shared size-dependent HDR/SDK resources require an owner drain; this
		// is a concrete resize section, never a recording-worker wait.
		_resources.Device->GetCommandQueue()->Flush();
		_deferred.Resize();

		_forceReset = true;
	}
	auto& grid = _deferred._grids.at(_slot);
	frame.GridLayout = BuildLightGridConstants(
	    _common.CameraProjection, _common.CameraView, _common.ActiveCameraNearPlane, _common.ActiveCameraFarPlane,
	    frame.Width, frame.Height, _resources.LocalLights.size(), _gridSettings);
	const auto& size = frame.GridLayout.Size;
	const UINT words = UINT((std::max)(size_t(1), size_t(size.x) * size.y * size.z * size.w));
	grid.Masks->Resize(words);
	grid.Constants->CopyData(0, frame.GridLayout);
}
void SceneLighting::AppendDeferred(Graph& graph, std::vector<ID3D12Resource*>& bindings, SceneGeometry& geometry,
                                   SceneEnvironment& environment)
{
	auto& scene = geometry._slots.at(_slot);
	auto& frame = _frames.at(_slot);
	auto& constants = *_resources.FrameConstants.at(_slot);
	auto& work = _work;
	const auto resource = [&](GDX12Resource* value)
	{ return NativeWork::ResourceFor(graph, bindings, value->D3DResource.Get(), "lighting/SSR resource"); };
	const auto texture = [&](GDX12Texture* value, Mode mode, State state)
	{ return Access{resource(value->GetResource()), mode, state}; };
	const State read = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
	auto* heap = _resources.SRV_UAV_Heap->GetHeap().Get();
	auto* lights = constants.ShadowLights.get();
	auto& grid = _deferred._grids.at(_slot);
	auto* masks = grid.Masks.get();
	// Placed RT/UAV initialization has its own declared state and packet.
	auto* hdr = _deferred._color->GetResource()->D3DResource.Get();
	work.Add(graph, "initialize deferred HDR target",
	         {texture(_deferred._color.get(), Mode::Write, D3D12_RESOURCE_STATE_RENDER_TARGET)},
	         [hdr](auto* cmd) { cmd->DiscardResource(hdr, nullptr); });
	auto lighting = environment.Reads(graph, bindings);
	if (auto* skyView = environment.VisibleSkyView()) lighting.push_back(texture(skyView, Mode::Read, read));
	for (UINT i = 0; i < _deferred._inputs.size(); ++i)
	{
		lighting.push_back(texture(_deferred._inputs[i]->GetTexture(), Mode::Read, read));
	}

	lighting.push_back({resource(&lights->GetResource()), Mode::Read, read});
	lighting.push_back(texture(_reflections->Published(), Mode::Read, read));
	lighting.push_back({resource(&constants.ReflectionProbes->GetResource()), Mode::Read, read});
	lighting.push_back({resource(&masks->GetResource()), Mode::Read, read});
	lighting.push_back({resource(&grid.Constants->GetResource()), Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ});
	lighting.push_back({resource(&constants.ShadowCB->GetResource()), Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ});
	lighting.push_back({resource(&constants.SkyCB->GetResource()), Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ});
	auto& skyViewSampling = environment.SkyViewSampling();
	lighting.push_back({resource(&skyViewSampling.GetResource()), Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ});
	lighting.push_back({scene.FrameIds[0], Mode::Read, State(D3D12_RESOURCE_STATE_GENERIC_READ)});
	lighting.push_back({scene.FrameIds[1], Mode::Read, State(D3D12_RESOURCE_STATE_GENERIC_READ)});
	lighting.push_back(texture(_deferred._color.get(), Mode::Write, UnorderedAccess));
	// Environment outputs can occur in both the explicit pass inputs and LUT
	// reads; normalize identical physical reads, rejecting state disagreements.
	auto* root = _deferred._root.get();
	auto* pipeline = _deferred._pipeline.Get();
	std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 18> inputs{};
	for (UINT i = 0; i < 12; ++i)
	{
		inputs[i < 11 ? i : 14] = _deferred._inputs[i]->GetTexture()->GetSRV()->GPUHandle;
	}
	inputs[11] = lights->GetSRV()->GPUHandle;
	inputs[12] = _reflections->Radiance();
	inputs[13] = constants.ReflectionProbes->GetSRV()->GPUHandle;
	inputs[15] = _resources.SkyTransmittanceSRV.GPUHandle;
	inputs[16] = _resources.SkyViewSRV.GPUHandle;
	inputs[17] = masks->GetSRV()->GPUHandle;
	const std::array<D3D12_GPU_VIRTUAL_ADDRESS, 6> cbvs{
	    scene.Main, scene.Camera, constants.ShadowCB->GetElementAddress(0), constants.SkyCB->GetElementAddress(0),
	    grid.Constants->GetElementAddress(0), skyViewSampling.GetElementAddress(0)};
	const auto output = _deferred._color->GetUAV()->GPUHandle;
	const UINT width = frame.Width, height = frame.Height;

	work.Add(graph, "clustered deferred lighting", UniqueLightingAccesses(lighting),
	         [=](auto* cmd)
	         {
		         cmd->SetDescriptorHeaps(1, &heap);
		         cmd->SetComputeRootSignature(root->GetRootSignature().Get());
		         cmd->SetPipelineState(pipeline);
		         for (UINT i = 0; i < cbvs.size(); ++i)
		         {
			         cmd->SetComputeRootConstantBufferView(root->GetBRootParamIndex(i), cbvs[i]);
		         }
		         for (UINT i = 0; i < 18; ++i)
		         {
			         cmd->SetComputeRootDescriptorTable(root->GetTRootParamIndex(i), inputs[i]);
		         }
		         cmd->SetComputeRootDescriptorTable(root->GetURootParamIndex(0), output);
		         cmd->Dispatch((width + LightingTileSize - 1) / LightingTileSize,
		                       (height + LightingTileSize - 1) / LightingTileSize, 1);
	         });
}
GDX12Texture* SceneLighting::AppendSSR(Graph& graph, std::vector<ID3D12Resource*>& bindings, SceneGeometry& geometry,
                                       SceneEnvironment& environment)
{
	if (!_ssrSettings.Enabled)
	{
		_forceReset = true;
		return _deferred._color.get();
	}
	auto& scene = geometry._slots.at(_slot);
	auto& frame = _frames.at(_slot);
	auto& work = _work;
	const auto resource = [&](GDX12Resource* value)
	{ return NativeWork::ResourceFor(graph, bindings, value->D3DResource.Get(), "lighting/SSR resource"); };
	const auto texture = [&](GDX12Texture* value, Mode mode, State state)
	{ return Access{resource(value->GetResource()), mode, state}; };
	const State read = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
	auto* heap = _resources.SRV_UAV_Heap->GetHeap().Get();
	const UINT width = frame.Width, height = frame.Height;
	auto& ssr = _ssr;
	const std::vector<IRenderPassLink*> ssrInputs{_deferred._color.get(), scene.DepthView.get(), scene.Color[2].get(),
	                                              scene.Color[3].get(),   scene.Color[1].get(),  environment.Output(1),
	                                              environment.Output(2)};
	if (!ssr)
	{
		ssr = std::make_unique<GDX12SSSRPass>();
		ssr->Setup(&_resources, &_common);
		ssr->SetInputs(ssrInputs);
		ssr->Initialize();
	}
	else
	{
		ssr->SetInputs(ssrInputs);
		if (ssr->_color.GetWidth() != width || ssr->_color.GetHeight() != height)
		{
			ssr->Resize();
			_forceReset = true;
		}
	}
	auto& p = *ssr;
	const auto settings = _ssrSettings;
	const bool settingsChanged = settings != p._previousSettings;
	if (settingsChanged)
	{
		_common.ResetUpscalerHistory = true;
	}
	const Matrix view = _common.CameraView, projection = _common.CameraProjection, viewProjection = view * projection;
	const XMFLOAT2 jitter{_common.ActiveCameraJitterOffsetX, _common.ActiveCameraJitterOffsetY};
	const auto history = _common.HistoryRevision;
	// Frame-local GBuffer addresses rotate with frame slots. They are not
	// history discontinuities. The SDK's private denoiser history persists
	// across those imports and is retired/recreated only on real resets.
	bool contextCreated = false;
	if (!p._context || _forceReset || settingsChanged || p._historyRevision != history ||
	    p._environmentRevision != _resources.ReflectionEnvironmentRevision)
	{
		p.CreateContext();
		contextCreated = true;
		p._previousViewProjection = viewProjection;
		p._previousJitter = jitter;
	}
	GDX12SSSRPass::Constants parameters;
	parameters.JitterDeltaUV = {(p._previousJitter.x - jitter.x) / width, (p._previousJitter.y - jitter.y) / height};
	parameters.RoughnessCutoff = settings.RoughnessCutoff;
	p._constants->CopyData(_slot, parameters);
	auto* ssrRoot = p._root.get();
	auto* prepare = p._prepare.Get();
	auto* resolve = p._resolve.Get();
	const auto camera = scene.Camera, ssrCB = p._constants->GetElementAddress(_slot);
	std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 11> srvs{};
	for (UINT i = 0; i < 7; ++i)
	{
		srvs[i] = p._inputs[i]->GetTexture()->GetSRV()->GPUHandle;
	}
	srvs[7] = p._radiance.GetSRV()->GPUHandle;
	srvs[8] = p._fallback.GetSRV()->GPUHandle;
	auto& probeData = *_resources.FrameConstants[_slot]->ReflectionProbes;
	srvs[9] = _reflections->Radiance();
	srvs[10] = probeData.GetSRV()->GPUHandle;

	const auto bind = [=](auto* cmd, bool resolvedInputs)
	{
		cmd->SetDescriptorHeaps(1, &heap);
		cmd->SetComputeRootSignature(ssrRoot->GetRootSignature().Get());
		cmd->SetComputeRootConstantBufferView(ssrRoot->GetBRootParamIndex(0), camera);
		cmd->SetComputeRootConstantBufferView(ssrRoot->GetBRootParamIndex(1), ssrCB);
		for (UINT i = 0; i < srvs.size(); ++i)
		{
			if (resolvedInputs || (i != 7 && i != 8))
			{
				cmd->SetComputeRootDescriptorTable(ssrRoot->GetTRootParamIndex(i), srvs[i]);
			}
		}
	};
	std::vector<Access> ssrReads;
	for (auto* input : p._inputs)
	{
		ssrReads.push_back(texture(input->GetTexture(), Mode::Read, read));
	}

	ssrReads.push_back({scene.FrameIds[1], Mode::Read, State(D3D12_RESOURCE_STATE_GENERIC_READ)});
	ssrReads.push_back({resource(&p._constants->GetResource()), Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ});
	auto prepareAccess = ssrReads;
	prepareAccess.push_back(texture(_reflections->Published(), Mode::Read, read));
	prepareAccess.push_back({resource(&probeData.GetResource()), Mode::Read, read});
	prepareAccess.push_back(texture(&p._roughness, Mode::Write, UnorderedAccess));
	prepareAccess.push_back(texture(&p._motion, Mode::Write, UnorderedAccess));
	prepareAccess.push_back(texture(&p._fallback, Mode::Write, UnorderedAccess));
	const auto roughness = p._roughness.GetUAV()->GPUHandle, motion = p._motion.GetUAV()->GPUHandle,
	           fallback = p._fallback.GetUAV()->GPUHandle;
	work.Add(graph, "SSSR prepare roughness/motion/environment fallback", UniqueLightingAccesses(prepareAccess),
	         [=](auto* cmd)
	         {
		         bind(cmd, false);
		         cmd->SetPipelineState(prepare);
		         cmd->SetComputeRootDescriptorTable(ssrRoot->GetURootParamIndex(0), roughness);
		         cmd->SetComputeRootDescriptorTable(ssrRoot->GetURootParamIndex(1), motion);
		         cmd->SetComputeRootDescriptorTable(ssrRoot->GetURootParamIndex(3), fallback);
		         cmd->Dispatch((width + LightingTileSize - 1) / LightingTileSize,
		                       (height + LightingTileSize - 1) / LightingTileSize, 1);
	         });
	FfxSssrDispatchDescription desc{};
	desc.color = Import(_deferred._color.get());
	desc.depth = Import(scene.DepthView.get());
	desc.normal = Import(scene.Color[2].get());
	desc.materialParameters = Import(&p._roughness, FFX_RESOURCE_STATE_COMPUTE_READ);
	desc.motionVectors = Import(&p._motion, FFX_RESOURCE_STATE_COMPUTE_READ);
	desc.environmentMap = Import(&p._fallback, FFX_RESOURCE_STATE_COMPUTE_READ);
	desc.brdfTexture = Import(environment.Output(2)->GetTexture());
	desc.output = Import(&p._radiance, FFX_RESOURCE_STATE_COMPUTE_READ);
	MatrixBytes(desc.invViewProjection, viewProjection.Invert());
	MatrixBytes(desc.projection, projection);
	MatrixBytes(desc.invProjection, projection.Invert());
	MatrixBytes(desc.view, view);
	MatrixBytes(desc.invView, view.Invert());
	MatrixBytes(desc.prevViewProjection, p._previousViewProjection);
	desc.renderSize = {width, height};
	desc.motionVectorScale = {1, 1};
	desc.iblFactor = 1;
	desc.normalUnPackMul = 1;
	desc.isRoughnessPerceptual = false;
	desc.temporalStabilityFactor = settings.TemporalStability;
	desc.depthBufferThickness = settings.Thickness;
	desc.roughnessThreshold = settings.RoughnessCutoff * settings.RoughnessCutoff;
	desc.varianceThreshold = settings.VarianceThreshold;
	desc.maxTraversalIntersections = settings.MaxIntersections;
	desc.minTraversalOccupancy = settings.MinOccupancy;
	desc.mostDetailedMip = (std::min)(UINT(settings.MostDetailedMip), UINT(std::log2((std::max)(width, height))));
	desc.samplesPerQuad = settings.SamplesPerQuad;
	desc.temporalVarianceGuidedTracingEnabled = settings.VarianceGuided;
	auto sdk = ssrReads;
	sdk.push_back(texture(&p._roughness, Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
	sdk.push_back(texture(&p._motion, Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
	sdk.push_back(texture(&p._fallback, Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
	sdk.push_back(texture(&p._radiance, Mode::ReadWrite, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
	auto* context = p._context.get();
	const auto sdkOperation =
	    work.Add(graph, "SSSR + denoiser (exclusive persistent SDK history)", UniqueLightingAccesses(sdk),
	             [context, desc](auto* cmd) mutable
	             {
		             desc.commandList = ffxGetCommandListDX12(cmd);
		             const auto result = ffxSssrContextDispatch(context, &desc);
		             if (result != FFX_OK)
		             {
			             throw std::runtime_error("Native SSSR SDK dispatch failed");
		             }
	             });
	// The bundled DX12 backend allocates/maps its fallback constant ring
	// on first ExecuteGpuJobs. Record that first real dispatch on the owner.
	// Subsequent dispatches reuse its private, queued-frame-safe ring.
	graph.Operations.at(sdkOperation).OwnerRecording = contextCreated;
	auto resolveAccess = ssrReads;
	resolveAccess.push_back(texture(&p._radiance, Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
	resolveAccess.push_back(texture(&p._fallback, Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
	resolveAccess.push_back(texture(&p._color, Mode::Write, UnorderedAccess));
	const auto color = p._color.GetUAV()->GPUHandle;
	work.Add(graph, "SSR specular composition", UniqueLightingAccesses(resolveAccess),
	         [=](auto* cmd)
	         {
		         bind(cmd, true);
		         cmd->SetPipelineState(resolve);
		         cmd->SetComputeRootDescriptorTable(ssrRoot->GetURootParamIndex(2), color);
		         cmd->Dispatch((width + LightingTileSize - 1) / LightingTileSize,
		                       (height + LightingTileSize - 1) / LightingTileSize, 1);
	         });
	p._previousViewProjection = viewProjection;
	p._previousJitter = jitter;
	p._historyRevision = history;
	p._environmentRevision = _resources.ReflectionEnvironmentRevision;
	p._previousSettings = settings;
	_forceReset = false;

	return &p._color;
}
void SceneLighting::AppendComposition(Graph& graph, std::vector<ID3D12Resource*>& bindings, SceneGeometry& geometry,
                                      SceneEnvironment& environment, GDX12Texture* opaqueHDR)
{
	_initialized = true;
	if (!_hdrContinuation)
	{
		throw std::logic_error("Opaque lighting requires a display continuation");
	}
	_transparency->SetOpaqueContinuation(
	    [&](auto& g, auto& b, auto* hdr, auto* depth, auto* motion)
	    {
		    _probePreview->Append(g, b, geometry, *_reflections, _slot, hdr, depth, motion, environment);
		    return _probePreview->HDR();
	    },
	    [this] { return _probePreview->Depth(); }, [this] { return _probePreview->Motion(); },
	    [this] { return _probePreview->ReadDepth(); });
	auto* hdr = _transparency->Append(graph, bindings, geometry, _slot, opaqueHDR, _sunShadows->Output(_slot),
	                                  _localShadows->Output(_slot), environment, *_reflections);
	_hdrContinuation(graph, bindings, hdr);
}
} // namespace BistroRender

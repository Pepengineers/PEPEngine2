#include "SceneLighting.h"
#include "Engine.UI/UI_test.h"
#include "SceneRemoteShadows.h"
#include "ShadowGeometry.h"
#include "Common/ConsoleVariables.h"
#include "Engine.Core/Logging/Logger.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include <FidelityFX/host/ffx_sssr.h>
#include <FidelityFX/host/backends/dx12/ffx_dx12.h>
#include <algorithm>
#include <cmath>
#include <cstring>
namespace BistroRender
{
namespace
{
AutoConsoleVariable<bool> AsyncLightGrid(L"Render.LightGrid.AsyncCompute", false,
                                        L"Build the light grid on the local COMPUTE queue.",
                                        EConsoleVariableFlags::Default);
AutoConsoleVariable<int> LightGridAdapter(L"Render.LightGrid.Adapter", -1,
    L"DXGI hardware index for Exact Bistro light grid construction; -1 uses GPU0",
    EConsoleVariableFlags::Default, [](int index) { return index >= -1; });
AutoConsoleVariable<int> SunShadowAdapter(L"Render.Shadows.Sun.Adapter", -1,
    L"DXGI hardware index for far CSM cascades; -1 uses GPU0", EConsoleVariableFlags::Default,
    [](int index) { return index >= -1; });
AutoConsoleVariable<UINT> SunShadowNearCount(L"Render.Shadows.Sun.NearCascades", 2,
    L"Number of nearest active sun cascades drawn on GPU0", EConsoleVariableFlags::Default,
    [](UINT count) { return count <= 4; });
AutoConsoleVariable<int> SunShadowSync(L"Render.Shadows.Sun.CrossSync", 1,
    L"Remote sun shadow freshness: 1 Exact, 3 Latest", EConsoleVariableFlags::Default,
    [](int mode) { return mode == 1 || mode == 3; });

constexpr UINT LightGridThreads = 64;
constexpr UINT LightingTileSize = 8;
constexpr UINT MaxDispatchGroups = D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION;
struct LightGridBuildParameters
{
	UINT Width, Height, LightCount, Groups;
	XMFLOAT4 Projection;
	float FarPlane, Padding[3]{};
	XMFLOAT4X4 View;
	GDX12LightGridConstants Layout;
};
static_assert(sizeof(LightGridBuildParameters) == 36 * sizeof(UINT));
static_assert(offsetof(LightGridBuildParameters, View) == 48 && offsetof(LightGridBuildParameters, Layout) == 112);

LightGridBuildParameters GridParameters(UINT width, UINT height, UINT count, const GDX12LightGridConstants& layout,
    const RenderPipelineCommonData& common, const GDX12CameraConstants& camera)
{
	const auto size = layout.Size;
	const UINT groups = size.w ? ((size.x * size.y + LightGridThreads - 1) / LightGridThreads) * size.z * size.w : 1;
	const auto& projection = common.CameraProjection;
	return {width, height, count, groups, {projection._11, projection._22, projection._31, projection._32},
	        common.ActiveCameraFarPlane, {}, camera.View, layout};
}

FfxResource Import(GDX12Texture* texture, FfxResourceStates state = FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ)
{
	auto* resource = texture->GetResource()->D3DResource.Get();
	return ffxGetResourceDX12(resource, ffxGetResourceDescriptionDX12(resource), L"Bistro SSSR", state);
}
void MatrixBytes(float* target, const Matrix& value)
{
	std::memcpy(target, &value, sizeof(value));
}
} // namespace

std::vector<Access> SceneLighting::UniqueLightingAccesses(std::vector<Access> accesses)
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

struct SceneLighting::LightGridKernel
{
	LightGridKernel(GDX12Device& device, bool compact)
	{
		GDX12RootSignatureDesc root;
		root.Constants = {sizeof(LightGridBuildParameters) / sizeof(UINT)};
		root.NumSingleSRVSlots = 1;
		root.NumSingleUAVSlots = 1;
		Root = std::make_unique<GDX12RootSignature>(&device, root);
		D3D12_FEATURE_DATA_D3D12_OPTIONS1 options{};
		const bool waves = SUCCEEDED(device.GetDevice()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options,
		                                                                    sizeof(options))) &&
		                   options.WaveOps;
		const D3D_SHADER_MACRO defines[] = {
		    {"LIGHT_GRID_WAVE_OPS", waves ? "1" : "0"}, {"LIGHT_GRID_COMPACT_INPUT", compact ? "1" : "0"}, {nullptr, nullptr}};
		Build = GDX12RenderHelpers::CreateComputePipeline(&device, Root.get(), SHADERS_FOLDER "LightGrid.hlsl", "BuildCS", defines);
	}

	auto Record(LightGridBuildParameters parameters, GDX12DescriptorHeap& descriptors,
	            D3D12_GPU_DESCRIPTOR_HANDLE lights, D3D12_GPU_DESCRIPTOR_HANDLE masks) const
	{
		auto* heap = descriptors.GetHeap().Get();
		return [this, heap, parameters, lights, masks](auto* cmd)
		{
			cmd->SetDescriptorHeaps(1, &heap);
			cmd->SetComputeRootSignature(Root->GetRootSignature().Get());
			cmd->SetPipelineState(Build.Get());
			cmd->SetComputeRoot32BitConstants(0, sizeof(parameters) / sizeof(UINT), &parameters, 0);
			cmd->SetComputeRootDescriptorTable(Root->GetTRootParamIndex(0), lights);
			cmd->SetComputeRootDescriptorTable(Root->GetURootParamIndex(0), masks);
			cmd->Dispatch((std::min)(parameters.Groups, MaxDispatchGroups),
			              (parameters.Groups + MaxDispatchGroups - 1) / MaxDispatchGroups, 1);
		};
	}

	std::unique_ptr<GDX12RootSignature> Root;
	ComPtr<ID3D12PipelineState> Build;
};

struct SceneLighting::RemoteLightGrid
{
	struct Slot
	{
		Slot(GDX12Device& device, GDX12DescriptorHeap& descriptors)
		    : Upload(&device, 1, EBufferType::Upload, false), Spheres(&device, 1, EBufferType::Default, false),
		      Masks(&device, 1, EBufferType::Default, false)
		{
			Spheres.CreateSRV(&descriptors, descriptors.GetAvailableIndex());
			Masks.CreateUAV(&descriptors, descriptors.GetAvailableIndex());
		}
		GDX12UploadBuffer<XMFLOAT4> Upload, Spheres;
		GDX12UploadBuffer<UINT> Masks;
		QueueSubmission::TransferBuffer Transfer;
		UINT64 Capacity = 0, Bytes = 0, PresentedFrame = 0;
		UINT LightCount = 0;
		XMUINT4 Layout{};
		Id Import = Invalid, Lighting = Invalid;
		CompletionToken Build, Export, Imported, Used, FrameDone;
	};

	RemoteLightGrid(const std::shared_ptr<QueueSubmission>& submit, Id owner, UINT slots)
	    : Owner(owner), Device(submit->Device(owner)), GPU(submit, owner, slots, 1),
	      Descriptors(&Device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2 * slots, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE),
	      Kernel(Device, true), Slots(slots)
	{
	}

	void Report()
	{
		if (Reported) return;
		const auto& submit = *GPU.Submissions();
		for (const auto& slot : Slots)
		{
			if (!slot || !slot->PresentedFrame || !submit.Complete(slot->FrameDone)) continue;
			Log::Info("RenderGraph", "Bistro Exact light grid Present S_OK frame=", slot->PresentedFrame,
			    " build=", submit.AdapterLabel(Owner), " deferred=", submit.AdapterLabel(0),
			    " lights=", slot->LightCount, " layout=", slot->Layout.x, "x", slot->Layout.y, "x", slot->Layout.z,
			    " wordsPerCell=", slot->Layout.w,
			    " cpuLightBytes=", UINT64(slot->LightCount) * sizeof(XMFLOAT4),
			    " cpuRootBytes=", sizeof(LightGridBuildParameters), " gpu0ToOwnerBytes=0 maskBytes=", slot->Bytes,
			    " buildFence=", slot->Build.Value, " exportFence=", slot->Export.Value,
			    " importFence=", slot->Imported.Value, " deferredFence=", slot->Used.Value,
			    " frameFence=", slot->FrameDone.Value);
			Reported = true;
			break;
		}
	}

	Id Owner;
	GDX12Device& Device;
	Executor GPU;
	GDX12DescriptorHeap Descriptors;
	LightGridKernel Kernel;
	std::vector<std::unique_ptr<Slot>> Slots;
	bool Reported = false;
};

SceneLighting::SceneLighting(GDX12DeviceResources& r, RenderPipelineCommonData& c)
    : _resources(r), _common(c), _aoPipelines(r.Device), _frames(r.FrameConstants.size())
{
	if (r.FrameConstants.size() > FFX_MAX_QUEUED_FRAMES)
	{
		throw std::logic_error("SSSR queued-frame contract exceeded");
	}
	_reflections = std::make_unique<SceneReflections>(r, c);
	_sunShadows = std::make_unique<SceneSunShadows>(r, c);
	{
		_localShadows = std::make_unique<SceneLocalShadows>(r, c);
		_staticShadows = std::make_unique<SceneLocalShadows>(r, c, true);
		_probePreview = std::make_unique<SceneProbePreview>(r, c);
		_transparency = std::make_unique<SceneTransparency>(r, c);
		_gridKernel = std::make_unique<LightGridKernel>(*r.Device, false);
	}
	_deferred._opaqueOnly = true;
	_deferred.SetFlag(RENDER_PASS_FLAG_USE_DOWNSCALED_RESOLUTION, true);
	Engine::UI::SetLocalShadowDistribution([this] { return LocalShadowDistribution(); });
}
SceneLighting::~SceneLighting()
{
	Engine::UI::SetLocalShadowDistribution({});
	_latestLocalShadows.clear();
	_latestSunShadows.fill(nullptr);
	for (auto& frame : _frames) frame.ShadowReads.clear();
	if (_ssr)
	{
		_ssr->ClearDenendencies();
	}
	if (_initialized)
	{
		_deferred.ClearDenendencies();
	}
}
void SceneLighting::UpdateLocalShadowProfile(Executor& primary)
{
	auto& submit = *primary.Submissions();
	const auto requested = GetLocalShadowProfile(_resources.ShadowQuality.LocalResolution);
	const UINT pages = _resources.ShadowQuality.LocalAtlasCount;
	_localShadowOwnerBusy = false;
	std::vector<Id> owners(pages);
	std::vector<ShadowCrossSync> sync(pages, ShadowCrossSync::Exact);
	for (UINT page = 0; requested.Face && page < pages; ++page)
	{
		const auto& adapters = _resources.LocalShadowPageAdapters;
		if (page < adapters.size()) owners[page] = submit.SelectAdapter(adapters[page]);
		if (owners[page])
		{
			const auto& configured = _resources.LocalShadowPageSync;
			if (page < configured.size()) sync[page] = configured[page];
		}
	}
	const UINT primaryPages = UINT(std::count(owners.begin(), owners.end(), Id(0)));
	const UINT activePages = requested.Face ? pages : 0;
	owners.resize(activePages);
	sync.resize(activePages);
	if (requested.Face == _localShadowProfile.Face && owners == _localPageOwners)
	{
		if (sync != _localPageSync) ++_localPlacementEpoch;
		_localPageSync = std::move(sync);
		return;
	}

	// Atlas descriptors and every old tile user retire before the replacement is allocated.
	if (_localShadowPages)
	{
		primary.Drain();
		RetireLocalShadows();
	}
	_localShadows->PrepareAtlases(requested.Page, activePages);
	_staticShadows->PrepareAtlases(requested.Page, activePages ? primaryPages : 0);
	_localShadowProfile = requested;
	_localShadowPages = activePages;
	_localPageOwners = std::move(owners);
	_localPageSync = std::move(sync);
	_localStaticSlices.assign(activePages, UINT_MAX);
	UINT slice = 0;
	for (UINT page = 0; page < activePages; ++page)
		if (!_localPageOwners[page]) _localStaticSlices[page] = slice++;
	++_localPlacementEpoch;
}

std::string SceneLighting::LocalShadowDistribution() const
{
	auto layout = std::string(_localShadowProfile.Name) + " (" + std::to_string(_localShadowProfile.Face) + ") | " +
	    _localShadows->Distribution(_slot);
	if (_localShadowOwnerBusy) layout += " | Local shadow owner busy (GPU0 fallback)";
	return layout;
}

LocalShadowTileCopy& SceneLighting::MainShadowTileCopy()
{
	if (!_localShadowTileCopy) _localShadowTileCopy = std::make_unique<LocalShadowTileCopy>(*_resources.Device);
	return *_localShadowTileCopy;
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
		frame.AO = std::make_unique<SceneAO>(_resources, _common, _aoPipelines, scene.DepthView.get(), normals);
	}
	frame.AO->Freeze(scene.DepthView.get(), normals, scene.Color[1].get(), geometry.PrepassNormals(slot), _aoTemporalIndex,
	                 submissions, aoDevice, _aoHistory);

	auto& constants = *_resources.FrameConstants.at(slot);
	auto vegetation = geometry.VegetationPrimitives();
	// The static cache reads the change journals before the main cache consumes them.
	auto staticVegetation = vegetation;
	staticVegetation.erase(std::remove_if(staticVegetation.begin(), staticVegetation.end(),
	                                      [](const auto& primitive) { return primitive.Dynamic; }), staticVegetation.end());
	_staticShadows->Freeze(slot, staticVegetation, frame.Width, frame.Height);
	_effectiveLocalPageOwners = _localPageOwners;
	_localShadows->Freeze(slot, vegetation, frame.Width, frame.Height);
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
	_localShadows->Allocate(slot, *_staticShadows, _localShadowProfile.Face, _localPageSync);
	_localShadows->Complete(slot);
	auto& lightSource = _localShadows->LightSource(slot);
	if (!lightSource.GetSRV())
		lightSource.CreateSRV(_resources.SRV_UAV_Heap.get(), _resources.SRV_UAV_Heap->GetAvailableIndex(SceneDescriptorStart));
	frame.LightCount = UINT(_localShadows->_slots.at(slot).Lights.size());
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

void SceneLighting::UpdateLightGridOwner(Executor& primary)
{
	const auto& submit = primary.Submissions();
	const Id owner = submit->SelectAdapter(LightGridAdapter.GetValue());
	if (owner == (_remoteGrid ? _remoteGrid->Owner : 0)) return;
	if (_remoteGrid)
	{
		primary.Drain();
		DrainLightGrid();
		_remoteGrid.reset();
	}
	if (owner)
		_remoteGrid = std::make_unique<RemoteLightGrid>(submit, owner, UINT(_frames.size()));
	Log::Info("RenderGraph", "Bistro light grid execution GPU ", submit->AdapterLabel(owner));
}

void SceneLighting::DrainLightGrid()
{
	if (_remoteGrid) _remoteGrid->GPU.Drain();
}

Id SceneLighting::LightGridDevice() const
{
	return _remoteGrid ? _remoteGrid->Owner : 0;
}

void SceneLighting::AppendGpuLightGrid(Graph& graph, std::vector<ID3D12Resource*>& bindings)
{
	const auto& frame = _frames.at(_slot);
	const auto resource = [&](GDX12Resource& value)
	{ return NativeWork::ResourceFor(graph, bindings, value.D3DResource.Get(), "GPU light grid resource"); };
	auto& constants = *_resources.FrameConstants.at(_slot);
	auto& lights = *constants.ShadowLights;
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
	if (_remoteGrid || !frame.LightCount) return;
	auto& grid = _deferred._grids.at(_slot);
	const auto& camera = constants.CameraCB->GetCPUData()[_common.ActiveCameraCBufferIndex];
	const auto parameters = GridParameters(frame.Width, frame.Height, frame.LightCount, frame.GridLayout, _common, camera);
	const bool async = AsyncLightGrid.GetValue();
	const State lightRead = async ? State(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
	                              : State(D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
	_work.Add(graph, "GPU light grid masks",
	    {{resource(grid.Masks->GetResource()), Mode::Write, UnorderedAccess}, {sources, Mode::Read, lightRead}},
	    _gridKernel->Record(parameters, *_resources.SRV_UAV_Heap, lights.GetSRV()->GPUHandle, grid.Masks->GetUAV()->GPUHandle),
	    {}, async ? Queue::Compute : Queue::Direct);
}

void SceneLighting::SubmitLightGrid(const Schedule& primary, std::vector<PacketWait>& waits)
{
	const auto& frame = _frames.at(_slot);
	if (!_remoteGrid || !frame.LightCount) return;
	auto& owner = *_remoteGrid;
	auto& slot = *owner.Slots.at(_slot);
	const Id import = primary.PacketFor(slot.Import);
	Graph plan;
	plan.Device = owner.Owner;
	std::vector<ID3D12Resource*> bindings;
	NativeWork work;
	const auto resource = [&](ID3D12Resource* value, const char* name)
	{ return NativeWork::ResourceFor(plan, bindings, value, name); };
	auto* spheres = slot.Spheres.GetResource().D3DResource.Get();
	const Id lights = resource(spheres, "owner local-light spheres");
	if (frame.LightCount)
	{
		const auto* snapshot = _localShadows->LightSource(_slot).GetCPUData();
		for (UINT i = 0; i < frame.LightCount; ++i)
		{
			const auto& light = snapshot[i];
			slot.Upload.CopyData(i, {light.Position.x, light.Position.y, light.Position.z, light.Range});
		}
		auto* upload = slot.Upload.GetResource().D3DResource.Get();
		const UINT64 lightBytes = UINT64(frame.LightCount) * sizeof(XMFLOAT4);
		work.Add(plan, "upload current local-light spheres on owner",
		    {{resource(upload, "local-light sphere upload"), Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ},
		     {lights, Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST}},
		    [=](auto* cmd) { cmd->CopyBufferRegion(spheres, 0, upload, 0, lightBytes); });
	}
	auto* masks = slot.Masks.GetResource().D3DResource.Get();
	const Id maskResource = resource(masks, "owner local-light masks");
	const auto& camera = _resources.FrameConstants.at(_slot)->CameraCB->GetCPUData()[_common.ActiveCameraCBufferIndex];
	const auto parameters = GridParameters(frame.Width, frame.Height, frame.LightCount, frame.GridLayout, _common, camera);
	const Id build = work.Add(plan, "owner build Exact light grid",
	    {{lights, Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE}, {maskResource, Mode::Write, UnorderedAccess}},
	    owner.Kernel.Record(parameters, owner.Descriptors, slot.Spheres.GetSRV()->GPUHandle, slot.Masks.GetUAV()->GPUHandle));
	auto* shared = slot.Transfer.Resources[1].Get();
	const UINT64 bytes = slot.Bytes;
	const Id publish = work.Add(plan, "owner export Exact light grid masks",
	    {{maskResource, Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE},
	     {resource(shared, "shared light grid masks"), Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST}},
	    [=](auto* cmd) { cmd->CopyBufferRegion(shared, 0, masks, 0, bytes); });
	plan.Operations[publish].TransferBytes = bytes;
	const auto schedule = Compile(plan);
	owner.GPU.Reserve(schedule);
	owner.GPU.Execute(schedule, bindings, [&work](Id id, UINT, auto* cmd) {
		if (!work.Record(id, cmd)) throw std::logic_error("Missing Exact light grid work");
	});
	const auto& completions = owner.GPU.Completions();
	slot.Build = completions.at(schedule.PacketFor(build));
	slot.Export = completions.at(schedule.PacketFor(publish));
	waits.push_back({import, slot.Export});
}

void SceneLighting::LightGridPresented(uint64_t frame, const Schedule& schedule, const std::vector<CompletionToken>& completions)
{
	if (!_remoteGrid || _remoteGrid->Reported || !_frames.at(_slot).LightCount) return;
	auto& slot = *_remoteGrid->Slots.at(_slot);
	slot.PresentedFrame = frame;
	slot.Imported = completions.at(schedule.PacketFor(slot.Import));
	slot.Used = completions.at(schedule.PacketFor(slot.Lighting));
	slot.FrameDone = completions.back();
	_remoteGrid->Report();
}
void SceneLighting::AppendAO(Graph& graph, std::vector<ID3D12Resource*>& bindings)
{
	_frames.at(_slot).AO->Append(graph, bindings);
}
bool SceneLighting::Record(Id id, ID3D12GraphicsCommandList* commands) const
{
	return _work.Record(id, commands) || (_sunShadows->Record(_slot, id, commands)) ||
	       (_localShadows->Record(_slot, id, commands)) || (_staticShadows->Record(_slot, id, commands)) ||
	       (_probePreview->Record(id, commands)) || (_transparency->Record(id, commands)) ||
	       (_reflections->Record(id, commands)) || (_frames.at(_slot).AO && _frames.at(_slot).AO->Record(id, commands));
}
void SceneLighting::Submitted(uint64_t fence)
{
	_staticShadows->Submitted(_slot);
	_localShadows->Submitted(_slot);
	_reflections->Submitted(fence);
}
GDX12Texture* SceneLighting::Append(Graph& graph, std::vector<ID3D12Resource*>& bindings, SceneGeometry& geometry,
                                    SceneEnvironment& environment)
{
	_work.Commands.clear();
	_frames.at(_slot).AO->AppendRemoteDepth(graph, bindings);
	AppendShadows(graph, bindings, geometry, environment);
	PrepareDeferred(geometry, environment);
	AppendAO(graph, bindings);
	AppendDeferred(graph, bindings, geometry, environment);
	auto* opaqueHDR = AppendSSR(graph, bindings, geometry, environment);
	return AppendComposition(graph, bindings, geometry, environment, opaqueHDR);
}
void SceneLighting::AppendShadows(Graph& graph, std::vector<ID3D12Resource*>& bindings, SceneGeometry& geometry,
                                  SceneEnvironment& environment)
{
	auto& scene = geometry._slots.at(_slot);
	auto& constants = *_resources.FrameConstants.at(_slot);
	const auto appendGrass = [&](const GeometryTarget& view, Graph& g, std::vector<ID3D12Resource*>& resources)
	{ geometry.AppendVegetationView(g, resources, _slot, view); };
	const std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 4> descriptors{scene.Buffers[0].Srv.GPUHandle,
	    scene.Buffers[1].Srv.GPUHandle, scene.Buffers[2].Srv.GPUHandle, scene.Buffers[3].Srv.GPUHandle};
	_sunShadows->Append(graph, bindings, _slot, descriptors, scene.Draws, scene.Geometry, appendGrass, ~_sunRemoteMask);
	for (const auto& view : _staticShadows->_slots[_slot].Views)
		if (_localPageOwners[view.Tile.Page]) _staticShadows->MarkDepthWritten(_slot, view.Light, view.Face);
	AppendRemoteShadows(graph, bindings, geometry);
	auto& statics = _staticShadows->_slots[_slot];
	auto& local = _localShadows->_slots[_slot];
	statics.Views.erase(std::remove_if(statics.Views.begin(), statics.Views.end(), [&](const auto& view)
	{
		if (!_localPageOwners[view.Tile.Page]) return false;
		if (!_effectiveLocalPageOwners[view.Tile.Page] &&
		    std::any_of(local.Views.begin(), local.Views.end(), [&](const auto& update) {
		        return update.Light == view.Light && update.Face == view.Face;
		    }))
			return false;
		statics.RemoteViews.push_back(view);
		return true;
	}), statics.Views.end());
	for (auto& view : statics.Views)
	{
		const UINT slice = _localStaticSlices[view.Tile.Page];
		view.DrawIntoFinal = slice == UINT_MAX; // Local fallback builds static depth directly in the final tile.
		if (!view.DrawIntoFinal) view.Tile.Page = slice;
	}
	if (statics.Views.empty())
		statics.Work.Commands.clear();
	else
	{
		// Exact fallback can append indirect commands while choosing the owner.
		_staticShadows->Complete(_slot);
		auto& staticLights = _staticShadows->LightSource(_slot);
		if (!staticLights.GetSRV())
			staticLights.CreateSRV(_resources.SRV_UAV_Heap.get(), _resources.SRV_UAV_Heap->GetAvailableIndex(SceneDescriptorStart));
		_staticShadows->Append(graph, bindings, _slot, descriptors, scene.Geometry,
		    staticLights, appendGrass, nullptr, {}, local.Depth.get());
	}
	if (!local.Views.empty())
	{
		std::vector<LocalShadowTileCopy::Region> copies;
		for (const auto& view : local.Views)
		{
			auto from = view.Tile;
			from.Page = _localStaticSlices[from.Page];
			if (from.Page != UINT_MAX)
				copies.push_back({from, view.Tile, local.Depth->Dsvs[view.Tile.Page]->CPUHandle});
		}
		if (!copies.empty())
			MainShadowTileCopy().Draw(graph, bindings, _work,
			    statics.Depth->Texture.GetResource()->D3DResource.Get(), local.Depth->Texture.GetResource()->D3DResource.Get(),
			    _resources.SRV_UAV_Heap->GetHeap().Get(), statics.Depth->Texture.GetSRV()->GPUHandle, std::move(copies),
			    "restore static shadow tiles");
	}
	_localShadows->Append(graph, bindings, _slot, descriptors,
	                      scene.Geometry, *constants.ShadowLights, appendGrass);
	_reflections->Append(graph, bindings, geometry, environment, _slot);
}

void SceneLighting::AppendCascadeOwners(std::vector<std::pair<std::string, Id>>& owners) const
{
	if (_resources.Lighting.SunShadowFlags & ShadowEnabled)
		for (UINT i = 0; i < _resources.ShadowSettings.CascadeCount; ++i)
			owners.emplace_back("CSM " + std::to_string(i), (_sunRemoteMask & (1u << i)) ? _sunOwner : 0);
}

void SceneLighting::AppendPanelOwners(std::vector<std::pair<std::string, Id>>& owners) const
{
	AppendCascadeOwners(owners);
	for (UINT page = 0; page < _effectiveLocalPageOwners.size(); ++page)
		owners.emplace_back("Local shadow page " + std::to_string(page), _effectiveLocalPageOwners[page]);
	const auto& probes = _reflections->Owners();
	for (size_t i = 0; i < probes.size(); ++i) owners.emplace_back("Probe " + std::to_string(i), probes[i]);
}

UINT64 SceneLighting::AppendBenchmarkShadowOwners(std::vector<std::pair<std::string, Id>>& owners) const
{
	if ((_resources.Lighting.SunShadowFlags & ShadowEnabled) && _resources.ShadowSettings.CascadeCount)
		owners.emplace_back("SunShadowAdapter", _sunRemoteMask ? _sunOwner : 0);
	AppendCascadeOwners(owners);

	UINT64 texels = 0;
	std::array<uint8_t, MaxLocalShadowAtlasCount> pageSources;
	std::fill_n(pageSources.begin(), _localPageOwners.size(), uint8_t(0));
	const auto& local = _localShadows->_slots.at(_slot);
	for (UINT i = 0; i < local.Lights.size(); ++i)
	{
		const auto& cache = local.Cache[i];
		texels += UINT64(cache.Resolution) * cache.Resolution * __popcnt(cache.ReadyFaces);
		for (UINT face = 0; face < LocalShadowFaceCount(local.Lights[i]); ++face)
		{
			const UINT bit = 1u << face;
			if (!(cache.ReadyFaces & bit)) continue;
			pageSources[cache.Tiles[face].Page] |= cache.RemoteFaces & bit ? 2u : 1u;
		}
	}
	for (UINT page = 0; page < _localPageOwners.size(); ++page)
	{
		const auto role = "Local shadow page " + std::to_string(page);
		if (pageSources[page] & 1u) owners.emplace_back(role, 0);
		if (pageSources[page] & 2u) owners.emplace_back(role, _localPageOwners[page]);
	}
	return texels;
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
	    frame.Width, frame.Height, frame.LightCount, _gridSettings);
	const auto& size = frame.GridLayout.Size;
	frame.GridWords = UINT((std::max)(size_t(1), size_t(size.x) * size.y * size.z * size.w));
	grid.Masks->Resize(frame.GridWords);
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
	if (frame.LightCount) lighting.push_back({resource(&masks->GetResource()), Mode::Read, read});
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
	std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 19> inputs{};
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
	inputs[18] = environment.AddCloudReads(graph, bindings, lighting);
	const std::array<D3D12_GPU_VIRTUAL_ADDRESS, 6> cbvs{
	    scene.Main, scene.Camera, constants.ShadowCB->GetElementAddress(0), constants.SkyCB->GetElementAddress(0),
	    grid.Constants->GetElementAddress(0), skyViewSampling.GetElementAddress(0)};
	const auto output = _deferred._color->GetUAV()->GPUHandle;
	const UINT width = frame.Width, height = frame.Height;

	RemoteLightGrid::Slot* remoteSlot = nullptr;
	if (_remoteGrid && frame.LightCount)
	{
		auto& owner = *_remoteGrid;
		owner.GPU.Begin(_slot);
		owner.Report();
		auto& slot = owner.Slots.at(_slot);
		if (!slot) slot = std::make_unique<RemoteLightGrid::Slot>(owner.Device, owner.Descriptors);
		remoteSlot = slot.get();
		const UINT lightCapacity = (std::max)(1u, frame.LightCount);
		slot->Upload.Resize(lightCapacity);
		slot->Spheres.Resize(lightCapacity);
		slot->Masks.Resize(frame.GridWords);
		const UINT64 bytes = UINT64(frame.GridWords) * sizeof(UINT);
		if (slot->Capacity < bytes)
		{
			slot->Transfer = _submissions->CreateTransferBuffer(owner.Owner, bytes);
			slot->Capacity = bytes;
		}
		slot->PresentedFrame = 0;
		slot->Bytes = bytes;
		slot->LightCount = frame.LightCount;
		slot->Layout = frame.GridLayout.Size;
		auto* shared = slot->Transfer.Resources[0].Get();
		auto* destination = masks->GetResource().D3DResource.Get();
		const Id input = NativeWork::ResourceFor(graph, bindings, shared, "shared light grid masks");
		// The copy waits for the owner export. The first mask read depends on this write.
		slot->Import = work.Add(graph, "GPU0 import Exact light grid masks",
		    {{input, Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE},
		     {resource(&masks->GetResource()), Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST}},
		    [=](auto* cmd) { cmd->CopyBufferRegion(destination, 0, shared, 0, bytes); }, {}, Queue::Copy);
		graph.Operations[slot->Import].TransferBytes = bytes;
	}

	const Id deferred = work.Add(graph, "clustered deferred lighting", UniqueLightingAccesses(lighting),
	         [=](auto* cmd)
	         {
		         cmd->SetDescriptorHeaps(1, &heap);
		         cmd->SetComputeRootSignature(root->GetRootSignature().Get());
		         cmd->SetPipelineState(pipeline);
		         for (UINT i = 0; i < cbvs.size(); ++i)
		         {
			         cmd->SetComputeRootConstantBufferView(root->GetBRootParamIndex(i), cbvs[i]);
		         }
		         for (UINT i = 0; i < inputs.size(); ++i)
		         {
			         cmd->SetComputeRootDescriptorTable(root->GetTRootParamIndex(i), inputs[i]);
		         }
		         cmd->SetComputeRootDescriptorTable(root->GetURootParamIndex(0), output);
		         cmd->Dispatch((width + LightingTileSize - 1) / LightingTileSize,
		                       (height + LightingTileSize - 1) / LightingTileSize, 1);
	         });
	if (remoteSlot) remoteSlot->Lighting = deferred;
}
GDX12Texture* SceneLighting::AppendSSR(Graph& graph, std::vector<ID3D12Resource*>& bindings, SceneGeometry& geometry,
                                       SceneEnvironment& environment)
{
	if (!_ssrSettings.Enabled)
	{
		if (_ssr)
		{
			_ssr.reset(); // The SDK pass retires its DIRECT work before releasing its context and textures.
			_common.ResetUpscalerHistory = true;
		}
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
	const Matrix view = _common.CameraView, projection = _common.CameraProjection, viewProjection = view * projection;
	const XMFLOAT2 jitter{_common.ActiveCameraJitterOffsetX, _common.ActiveCameraJitterOffsetY};
	const auto history = _common.HistoryRevision;
	// Frame-local GBuffer addresses rotate with frame slots. They are not
	// history discontinuities. The SDK's private denoiser history persists
	// across those imports and is retired/recreated only on real resets.
	bool contextCreated = false;
	if (!p._context || _forceReset || p._historyRevision != history ||
	    p._environmentRevision != _resources.ReflectionEnvironmentRevision)
	{
		p.CreateContext();
		_common.ResetUpscalerHistory = true;
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
	_forceReset = false;

	return &p._color;
}
GDX12Texture* SceneLighting::AppendComposition(Graph& graph, std::vector<ID3D12Resource*>& bindings, SceneGeometry& geometry,
                                               SceneEnvironment& environment, GDX12Texture* opaqueHDR)
{
	_initialized = true;
	_transparency->SetOpaqueContinuation(
	    [&](auto& g, auto& b, auto* hdr, auto* depth, auto* motion)
	    {
		    _probePreview->Append(g, b, geometry, *_reflections, _slot, hdr, depth, motion, environment);
		    return _probePreview->HDR();
	    },
	    [this] { return _probePreview->Depth(); }, [this] { return _probePreview->Motion(); });
	return _transparency->Append(graph, bindings, geometry, _slot, opaqueHDR, environment);
}
} // namespace BistroRender

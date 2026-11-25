#include "SceneLighting.h"
#include "Engine.UI/UI_test.h"
#include "ProbeInputMirror.h"
#include "LocalShadowTileCopy.h"
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

bool CompatibleLocalShadow(const GDX12LocalLightConstants& cached, const GDX12LocalLightConstants& current)
{
	return GetLocalShadowKey(cached) == GetLocalShadowKey(current) && cached.Type == current.Type &&
	       cached.ShadowRevision == current.ShadowRevision &&
	       cached.Range == current.Range &&
	       std::memcmp(&cached.Position, &current.Position, sizeof(current.Position)) == 0 &&
	       std::memcmp(cached.ShadowViewProj, current.ShadowViewProj, sizeof(current.ShadowViewProj)) == 0;
}

constexpr UINT WholeShadowPageCoveragePercent = 75;

bool CoversMostOfShadowPage(UINT64 texels, UINT side)
{
	return texels * 100 >= UINT64(side) * side * WholeShadowPageCoveragePercent;
}

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

Id LightGridPacket(const Schedule& schedule, Id operation)
{
	const auto found = std::find_if(schedule.Packets.begin(), schedule.Packets.end(),
	    [operation](const auto& packet) { return packet.Operation == operation; });
	if (operation == Invalid || found == schedule.Packets.end())
		throw std::logic_error("Exact light grid operation missing from schedule");
	return Id(found - schedule.Packets.begin());
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

struct SceneLighting::ShadowPublication
{
	struct LocalLight
	{
		GDX12LocalLightConstants Metadata;
		uint64_t Generation = 0;
		std::vector<std::pair<UINT, UINT>> Faces;
		std::array<uint64_t, 6> PageOrders{};
	};
	Id Owner = 0;
	UINT OriginSlot = 0;
	uint64_t Order = 0, LocalPlacement = 0, SunPlacement = 0, SceneRevision = 0, GrassRevision = 0;
	UINT LocalPage = UINT_MAX, SunResolution = 0, SunMask = 0, SunRevision = 0;
	GDX12ShadowConstants SunSettings{};
	std::map<UINT, LocalLight> Lights;
	std::shared_ptr<LocalShadowTileTransfer> Local;
	std::shared_ptr<LocalShadowPageTransfer> Page;
	std::shared_ptr<SunShadowTileTransfer> Sun;
	bool LocalStaged = false, SunStaged = false;
};

struct SceneLighting::ShadowVertexBaseline
{
	bool CreatedForLocalShadows = false;
	ComPtr<ID3D12Resource> Source;
	GDX12ResourceHandle Data;
	uint64_t Version = 0;
	CompletionToken Ready;
};

struct SceneLighting::RemoteShadows
{
	// Masked materials use the primary bindless indices. The tail holds the mirrored vegetation
	// views (at most one per live input), draw descriptors and per-resolution shadow atlases.
	static UINT DescriptorCount(size_t liveInputs)
	{
		const size_t count = SceneDescriptorStart + liveInputs + 128;
		return UINT((std::min)((count + 1023) & ~size_t(1023), size_t(D3D12_MAX_SHADER_VISIBLE_DESCRIPTOR_HEAP_SIZE_TIER_1)));
	}
	RemoteShadows(const std::shared_ptr<QueueSubmission>& submit, Id owner, RenderPipelineCommonData& common,
	              const GDX12DeviceResources& primary, UINT descriptors)
	    : GPU(submit, owner, 1), Mirror(*submit, owner), Vegetation(Resources)
	{
		auto& device = submit->Device(owner);
		if (descriptors)
		{
			Resources.Device = &device;
			Resources.InputLayouts = primary.InputLayouts;
			Resources.SRV_UAV_Heap = std::make_unique<GDX12DescriptorHeap>(&device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
			    descriptors, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
			Local = std::make_unique<SceneLocalShadows>(Resources, common, false, true);
			Static = std::make_unique<SceneLocalShadows>(Resources, common, true, true);
		}
		else
			Resources.Initialize(&device);
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
	std::unique_ptr<SceneLocalShadows> Static;
	std::unique_ptr<SceneSunShadows> Sun;
	std::shared_ptr<SunShadowTileTransfer> SunTransfer;
	std::array<GDX12Descriptor, 4> SceneViews;
	std::unique_ptr<LocalShadowTileCopy> TileCopy;
	std::map<UINT, uint64_t> StaticVersions, PendingStaticVersions;
	std::map<std::pair<UINT, UINT>, uint64_t> FinalOrders;
	std::vector<std::shared_ptr<LocalShadowTileTransfer>> Transfers;
	std::vector<std::shared_ptr<LocalShadowPageTransfer>> PageTransfers;
	std::vector<std::shared_ptr<ShadowPublication>> Publications;
	Graph Plan;
	NativeWork Work;
	std::vector<ID3D12Resource*> Bindings;
	Id Export = Invalid, Import = Invalid, Input = Invalid;
	Id SeedVertices = Invalid, ExactExport = Invalid;
	CompletionToken Produced, ExactProduced, Imported, Consumer;
	std::shared_ptr<GDX12ResourceHandle> SkinInput;
	std::shared_ptr<ShadowVertexBaseline> VertexBase;
	ID3D12Resource* VertexSource = nullptr;
	struct VertexUpdate
	{
		uint64_t From = 0, To = 0;
		UINT64 TransferBytes = 0, LocalBytes = 0, BaselineBytes = 0;
		bool Seed = false, Clone = false;
	} Vertices;
	std::vector<UINT> Lights;
	UINT Count = 0;
	UINT SunMask = 0, NearMask = 0;
	uint64_t MeshRevision = UINT64_MAX, VertexDeformationRevision = 0, TextureRevision = UINT64_MAX;
	bool ReportedVertexBaseline = false, ReportedVertexDelta = false;
	bool Reported = false;
};

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
void SceneLighting::RetireLocalShadows()
{
	for (auto& frame : _frames)
	{
		for (auto it = frame.ShadowOwners.begin(); it != frame.ShadowOwners.end();)
		{
			if (it->first.Local)
			{
				it = frame.ShadowOwners.erase(it);
				continue;
			}
			auto& remote = *it->second;
			if (remote.VertexBase && remote.VertexBase->CreatedForLocalShadows)
			{
				remote.GPU.Drain();
				remote.VertexBase.reset();
			}
			++it;
		}
		frame.ShadowReads.clear();
	}
	_latestLocalShadows.clear();
	for (auto it = _shadowVertexBaselines.begin(); it != _shadowVertexBaselines.end();)
		if (it->second->CreatedForLocalShadows) it = _shadowVertexBaselines.erase(it);
		else ++it;
	_localShadows->ResetLayout();
	_staticShadows->ResetLayout();
	_localShadowPages = 0;
	_localPageOwners.clear();
	_localPageSync.clear();
	_effectiveLocalPageOwners.clear();
	_localStaticSlices.clear();
	_localShadowProfile = LocalShadowProfiles[0];
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
	const Id import = LightGridPacket(primary, slot.Import);
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
	const auto schedule = Compile(plan);
	owner.GPU.Reserve(schedule);
	owner.GPU.Execute(schedule, bindings, [&work](Id id, UINT, auto* cmd) {
		if (!work.Record(id, cmd)) throw std::logic_error("Missing Exact light grid work");
	});
	const auto& completions = owner.GPU.Completions();
	slot.Build = completions.at(LightGridPacket(schedule, build));
	slot.Export = completions.at(LightGridPacket(schedule, publish));
	waits.push_back({import, slot.Export});
}

void SceneLighting::LightGridPresented(uint64_t frame, const Schedule& schedule, const std::vector<CompletionToken>& completions)
{
	if (!_remoteGrid || _remoteGrid->Reported || !_frames.at(_slot).LightCount) return;
	auto& slot = *_remoteGrid->Slots.at(_slot);
	slot.PresentedFrame = frame;
	slot.Imported = completions.at(LightGridPacket(schedule, slot.Import));
	slot.Used = completions.at(LightGridPacket(schedule, slot.Lighting));
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
	const std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 4> descriptors{scene.Buffers[0].Srv.GPUHandle,
	    scene.Buffers[1].Srv.GPUHandle, scene.Buffers[2].Srv.GPUHandle, scene.Buffers[3].Srv.GPUHandle};
	_sunShadows->Append(graph, bindings, _slot, descriptors, scene.Draws, scene.Geometry, appendGrass, ~_sunRemoteMask);
	for (const auto& view : _staticShadows->_slots[_slot].Views)
		if (_localPageOwners[view.Tile.Page]) _staticShadows->MarkDepthWritten(_slot, view.Light, view.Face);
	AppendRemoteLocalShadows(graph, bindings, geometry);
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

void SceneLighting::AppendPanelOwners(std::vector<std::pair<std::string, Id>>& owners) const
{
	if (_resources.Lighting.SunShadowFlags & ShadowEnabled)
		for (UINT i = 0; i < _resources.ShadowSettings.CascadeCount; ++i)
			owners.emplace_back("CSM " + std::to_string(i), (_sunRemoteMask & (1u << i)) ? _sunOwner : 0);
	for (UINT page = 0; page < _effectiveLocalPageOwners.size(); ++page)
		owners.emplace_back("Local shadow page " + std::to_string(page), _effectiveLocalPageOwners[page]);
	const auto& probes = _reflections->Owners();
	for (size_t i = 0; i < probes.size(); ++i) owners.emplace_back("Probe " + std::to_string(i), probes[i]);
}

void SceneLighting::AppendBenchmarkShadowOwners(std::vector<std::pair<std::string, Id>>& owners) const
{
	if ((_resources.Lighting.SunShadowFlags & ShadowEnabled) && _resources.ShadowSettings.CascadeCount)
		owners.emplace_back("SunShadowAdapter", _sunRemoteMask ? _sunOwner : 0);

	std::vector<UINT> pageSources(_localPageOwners.size());
	const auto& local = _localShadows->_slots.at(_slot);
	for (UINT i = 0; i < local.Lights.size(); ++i)
	{
		const auto& cache = local.Cache[i];
		for (UINT face = 0; face < LocalShadowFaceCount(local.Lights[i]); ++face)
		{
			const UINT bit = 1u << face;
			if (!(cache.ReadyFaces & bit)) continue;
			pageSources[cache.Tiles[face].Page] |= cache.RemoteFaces & bit ? 2u : 1u;
		}
	}
	for (UINT page = 0; page < pageSources.size(); ++page)
	{
		const auto role = "Local shadow page " + std::to_string(page);
		if (pageSources[page] & 1u) owners.emplace_back(role, 0);
		if (pageSources[page] & 2u) owners.emplace_back(role, _localPageOwners[page]);
	}
}

void SceneLighting::AppendRemoteLocalShadows(Graph& primary, std::vector<ID3D12Resource*>& primaryBindings,
                                            SceneGeometry& geometry)
{
	auto& source = _localShadows->_slots.at(_slot);
	auto& owners = _frames.at(_slot).ShadowOwners;
	auto& sun = _sunShadows->_slots.at(_slot);
	const auto localAtlas = [&]() { return source.Depth->Texture.GetResource()->D3DResource.Get(); };
	const auto makeRegion = [&](UINT packed, const GDX12LocalShadowTile& tile)
	{
		return LocalShadowTileCopy::Region{{packed, 0, 0, tile.Side}, tile, source.Depth->Dsvs[tile.Page]->CPUHandle};
	};
	const auto sunAtlas = sun.Atlas.GetResource()->D3DResource;
	const auto sunSettings = _resources.ShadowSettings;
	auto& frame = _frames.at(_slot);
	// Begin has retired this primary slot. Other slots retain the publications they read.
	const UINT completed = frame.CrossSlotShadows & ~_reportedCrossSlotShadows;
	if (completed && frame.ShadowConsumer && _submissions->Complete(frame.ShadowConsumer))
	{
		Log::Info("RenderGraph", "GPU0 completed cross-slot shadow publication consumer fence=", frame.ShadowConsumer.Value,
		          " slot=", _slot, " local=", bool(completed & 1u), " CSM=", bool(completed & 2u));
		_reportedCrossSlotShadows |= completed;
	}
	frame.CrossSlotShadows = 0;
	frame.ShadowReads.clear();
	if (frame.LatestSunAtlas.Get() != sunAtlas.Get())
		frame.SunShadowOrders.fill(0);
	frame.LatestSunAtlas = sunAtlas;

	const auto compatibleSun = [&](const ShadowPublication& publication)
	{
		return _sunSync == 3 && publication.Sun && publication.Owner == _sunOwner &&
		       publication.SunPlacement == _sunPlacementEpoch &&
		       publication.SunResolution == _resources.ShadowQuality.SunResolution &&
		       std::memcmp(&publication.SunSettings, &sunSettings, sizeof(sunSettings)) == 0;
	};
	const auto compatibleLocalDepth = [&](const ShadowPublication& publication, UINT index, UINT face)
	    -> const ShadowPublication::LocalLight*
	{
		const auto& light = source.Lights[index];
		const auto page = light.ShadowTiles[face].Page;
		const auto found = publication.Lights.find(light.EntityId);
		return page < _localPageOwners.size() && _localPageOwners[page] == publication.Owner &&
		       publication.LocalPlacement == _localPlacementEpoch && found != publication.Lights.end() &&
		       found->second.Metadata.ShadowTiles[face].Side == light.ShadowTiles[face].Side &&
		       found->second.Generation == source.Cache[index].Generation &&
		       CompatibleLocalShadow(found->second.Metadata, light) ? &found->second : nullptr;
	};
	const auto compatibleLocal = [&](const ShadowPublication& publication, UINT index, UINT face)
	{
		return (source.Cache[index].LatestFaces & (1u << face)) && compatibleLocalDepth(publication, index, face);
	};
	for (auto it = _latestLocalShadows.begin(); it != _latestLocalShadows.end();)
	{
		const auto light = std::lower_bound(source.Lights.begin(), source.Lights.end(), it->first,
		    [](const auto& item, UINT id) { return item.EntityId < id; });
		if (light == source.Lights.end() || light->EntityId != it->first || !source.Cache[light - source.Lights.begin()].LatestFaces)
			it = _latestLocalShadows.erase(it);
		else
		{
			for (UINT face = 0; face < it->second.size(); ++face)
				if (it->second[face] && !compatibleLocal(*it->second[face], UINT(light - source.Lights.begin()), face))
					it->second[face].reset();
			++it;
		}
	}
	for (auto& cascade : _latestSunShadows)
		if (cascade && cascade->SunPlacement != _sunPlacementEpoch) cascade.reset();

	for (auto& candidateFrame : _frames)
		for (auto& [lane, remote] : candidateFrame.ShadowOwners)
		{
			if (remote->Publications.empty() || !remote->Produced || !_submissions->Complete(remote->Produced)) continue;
			for (const auto& publication : remote->Publications)
			{
				for (const auto& [entity, light] : publication->Lights)
				{
					const auto active = std::lower_bound(source.Lights.begin(), source.Lights.end(), entity,
					    [](const auto& item, UINT id) { return item.EntityId < id; });
					if (active == source.Lights.end() || active->EntityId != entity) continue;
					const UINT index = UINT(active - source.Lights.begin());
					for (const auto& [slice, face] : light.Faces)
					{
						if (!compatibleLocal(*publication, index, face)) continue;
						auto& selected = _latestLocalShadows[entity][face];
						if (!selected || !compatibleLocal(*selected, index, face) || selected->Order < publication->Order)
							selected = publication;
					}
				}
				if (compatibleSun(*publication))
					for (UINT tile = 0; tile < _latestSunShadows.size(); ++tile)
					{
						if (!(publication->SunMask & (1u << tile))) continue;
						auto& selected = _latestSunShadows[tile];
						if (!selected || !compatibleSun(*selected) || selected->Order < publication->Order)
							selected = publication;
					}
			}
			remote->Publications.clear();
		}

	for (auto& [lane, remote] : owners)
	{
		remote->Export = remote->Import = Invalid;
		if (!remote->Reported && remote->Consumer && _submissions->Complete(remote->Consumer))
		{
			_submissions->LogFirstImport("local shadow depth", remote->ExactProduced, remote->Imported);
			for (const UINT light : remote->Lights)
				Log::Info("RenderGraph", "GPU0 completed local shadow consumer fence=", remote->Consumer.Value,
				          " entity=", light, " slices=", remote->Count, " owner=", _submissions->AdapterLabel(lane.Owner));
			if (remote->SunMask)
				Log::Info("RenderGraph", "GPU0 completed CSM consumer fence=", remote->Consumer.Value,
				          " GPU0 cascade mask=", remote->NearMask, " imported cascade mask=", remote->SunMask,
				          " owner=", _submissions->AdapterLabel(lane.Owner));
			remote->Reported = true;
		}
	}

	const auto currentCasters = [&](const ShadowPublication& publication)
	{
		return publication.SceneRevision == source.SceneRevision && publication.GrassRevision == source.GrassCasterRevision;
	};
	std::vector<UINT> available(source.Lights.size());
	std::vector<UINT> current(source.Lights.size());
	std::map<ShadowPublication*, std::vector<LocalShadowTileCopy::Region>> imports;
	for (UINT i = 0; i < source.Lights.size(); ++i)
	{
		const auto& light = source.Lights[i];
		if (!source.Cache[i].LatestFaces || light.ShadowFirstSlice == UINT_MAX) continue;
		const auto found = _latestLocalShadows.find(light.EntityId);
		if (found == _latestLocalShadows.end()) continue;
		const auto& faces = found->second;
		const UINT needed = light._padding[2];
		for (UINT face = 0; face < faces.size(); ++face)
		{
			if (!(needed & (1u << face)) || !faces[face] || !compatibleLocal(*faces[face], i, face)) continue;
			available[i] |= 1u << face;
			const auto& publication = faces[face];
			if (currentCasters(*publication)) current[i] |= 1u << face;
			auto& order = source.Cache[i].PublicationOrders[face];
			if (order >= publication->Order) continue;
			const auto& tiles = publication->Lights.at(light.EntityId).Faces;
			const auto tile = std::find_if(tiles.begin(), tiles.end(), [=](const auto& entry) { return entry.second == face; });
			if (tile == tiles.end()) throw std::logic_error("Missing published local shadow face");
			auto region = makeRegion(tile->first, light.ShadowTiles[face]);
			if (publication->Page)
			{
				region.From = publication->Lights.at(light.EntityId).Metadata.ShadowTiles[face];
				region.From.Page = 0;
			}
			imports[publication.get()].push_back(region);
			_localShadows->MarkDepthWritten(_slot, i, face);
			source.Cache[i].RemoteFaces |= 1u << face;
			order = publication->Order;
			frame.ShadowReads.push_back(publication);
		}
	}
	for (auto& [publication, regions] : imports)
	{
		if (publication->Page)
		{
			const UINT page = publication->LocalPage;
			UINT64 texels = 0;
			bool samePage = true;
			for (const auto& region : regions)
			{
				samePage &= region.To.Page == page;
				texels += UINT64(region.To.Side) * region.To.Side;
			}
			bool coherent = samePage && CoversMostOfShadowPage(texels, source.Depth->Side);
			for (UINT i = 0; coherent && i < source.Lights.size(); ++i)
			{
				const auto& light = source.Lights[i];
				for (UINT face = 0; coherent && face < LocalShadowFaceCount(light); ++face)
				{
					const auto& tile = light.ShadowTiles[face];
				const auto& cache = source.Cache[i];
				if (!((cache.ReadyFaces | cache.ResidentFaces) & (1u << face)) || tile.Page != page) continue;
					const auto* old = compatibleLocalDepth(*publication, i, face);
					coherent = old && old->Metadata.ShadowTiles[face] == tile && old->PageOrders[face] &&
					    old->PageOrders[face] == cache.PublicationOrders[face];
				}
			}
			if (coherent)
				publication->Page->Import(primary, primaryBindings, _work, localAtlas(), page);
			else
			{
				publication->Page->Publish(primary, primaryBindings, _work, MainShadowTileCopy(), localAtlas(), std::move(regions));
			}
		}
		else
		{
			if (!publication->LocalStaged)
			{
				publication->Local->Stage(primary, primaryBindings, _work);
				publication->LocalStaged = true;
			}
			publication->Local->Publish(primary, primaryBindings, _work, MainShadowTileCopy(), localAtlas(), std::move(regions));
		}
		if (publication->OriginSlot != _slot) frame.CrossSlotShadows |= 1u;
	}

	UINT sunCovered = 0;
	for (UINT tile = 0; tile < _latestSunShadows.size(); ++tile)
		if (_latestSunShadows[tile] && compatibleSun(*_latestSunShadows[tile])) sunCovered |= 1u << tile;
	const bool sunAvailable = _sunRemoteMask && (sunCovered & _sunRemoteMask) == _sunRemoteMask;
	if (sunAvailable)
	{
		std::map<ShadowPublication*, UINT> sunImports;
		for (UINT tile = 0; tile < _latestSunShadows.size(); ++tile)
		{
			const UINT bit = 1u << tile;
			if (!(_sunRemoteMask & bit)) continue;
			const auto& publication = _latestSunShadows[tile];
			if (frame.SunShadowOrders[tile] != publication->Order)
			{
				sunImports[publication.get()] |= bit;
				frame.SunShadowOrders[tile] = publication->Order;
				frame.ShadowReads.push_back(publication);
			}
			if (currentCasters(*publication) && publication->SunRevision == _resources.Lighting.SunShadowRevision &&
			    !(_resources.Lighting.SunShadowFlags & ShadowEveryFrame))
			{
				sun.DirtyCascades &= ~bit;
				sun.Update &= ~bit;
			}
		}
		for (const auto& [publication, mask] : sunImports)
		{
			if (!publication->SunStaged)
			{
				publication->Sun->Stage(primary, primaryBindings, _work);
				publication->SunStaged = true;
			}
			publication->Sun->Publish(primary, primaryBindings, _work, sun.Atlas, mask);
			if (publication->OriginSlot != _slot) frame.CrossSlotShadows |= 2u;
		}
	}

	struct Batch
	{
		std::vector<SceneLocalShadows::View> Views;
		bool Latest = false;
	};
	std::map<ShadowLane, std::vector<Batch>> views;
	const auto busy = [&](Id owner, bool local)
	{
		const auto found = owners.find({owner, !local, local});
		return found != owners.end() && !found->second->Publications.empty();
	};
	source.Views.erase(std::remove_if(source.Views.begin(), source.Views.end(), [&](const auto& view)
	{
		const Id owner = _localPageOwners[view.Tile.Page];
		if (!owner) return false;
		const bool latest = (available[view.Light] & (1u << view.Face)) != 0;
		if (latest && (current[view.Light] & (1u << view.Face)) && !(source.Lights[view.Light].ShadowFlags & ShadowEveryFrame))
		{
			source.RemoteViews.push_back(view);
			return true;
		}
		if (!latest || !busy(owner, true))
		{
			auto& batches = views[{owner, false}];
			const auto batch = std::find_if(batches.begin(), batches.end(), [&](const Batch& batch)
			{
				const auto& tile = batch.Views.front().Tile;
				return batch.Latest == latest && tile.Page == view.Tile.Page && tile.Side == view.Tile.Side &&
				       batch.Views.size() < MaxLocalShadowAtlasCount;
			});
			if (batch != batches.end()) batch->Views.push_back(view);
			else batches.push_back({{view}, latest});
		}
		return true;
	}), source.Views.end());
	if (!source.Depth)
		for (auto it = _shadowVertexBaselines.begin(); it != _shadowVertexBaselines.end();)
			if (it->second->CreatedForLocalShadows && it->second.use_count() == 1) it = _shadowVertexBaselines.erase(it);
			else ++it;
	const UINT sunUpdate = sun.Update & _sunRemoteMask;
	const ShadowLane sunLane{_sunOwner, sunAvailable, false};
	const bool orderSun = sunUpdate && (!sunAvailable || !busy(_sunOwner, false));
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
	std::map<Id, Id> baselineExports;
	for (auto& [lane, batches] : views)
	{
		const Id owner = lane.Owner;
		auto& ownerDevice = _submissions->Device(owner);
		auto& statics = _staticShadows->_slots.at(_slot);
		const auto fallback = [&]
		{
			for (const auto& batch : batches)
				if (!batch.Latest)
					for (const auto& view : batch.Views)
				{
					_effectiveLocalPageOwners[view.Tile.Page] = 0;
					auto& cache = source.Cache[view.Light];
					cache.LocalFallback |= 1u << view.Face;
					source.Views.push_back(view);
					cache.PublicationOrders[view.Face] = ++_shadowOrder;
					if (std::none_of(statics.Views.begin(), statics.Views.end(), [&](const auto& dirty) {
					        return dirty.Light == view.Light && dirty.Face == view.Face;
					    }))
						SceneLocalShadows::PackView(statics, statics.Cache[view.Light].Faces[view.Face],
						    view.Light, view.Face, view.Tile);
				}
		};
		const UINT descriptors = lane.Local ? RemoteShadows::DescriptorCount(live.size()) : 0;
		const auto existing = owners.find(lane);
		if (lane.Local && existing != owners.end() && existing->second->Produced &&
		    !_submissions->Complete(existing->second->Produced))
		{
			_localShadowOwnerBusy = true;
			fallback();
			continue;
		}
		if (existing != owners.end() && existing->second->Resources.SRV_UAV_Heap->GetNumDescriptors() < descriptors)
			owners.erase(existing); // This lane is idle. Rebind the mirrors in a larger heap.
		const auto previous = owners.find(lane);
		const auto* cached = previous == owners.end() ? nullptr : previous->second.get();
		std::vector<UINT> ownerSlices(_localPageOwners.size());
		UINT ownerPages = 0;
		for (UINT page = 0; lane.Local && page < _localPageOwners.size(); ++page)
			if (_localPageOwners[page] == owner) ownerSlices[page] = ownerPages++;
		struct PageUpdate
		{
			UINT64 Area = 0;
			bool Exact = false, Latest = false, Full = false, KeepPublication = false;
			std::set<std::pair<UINT, UINT>> Updated;
			std::vector<std::pair<UINT, UINT>> Readable;
		};
		std::map<UINT, PageUpdate> pageUpdates;
		const auto staticDirty = [&](const SceneLocalShadows::View& view)
		{
			const auto version = statics.Cache[view.Light].ContentVersions[view.Face];
			const auto key = view.Light * 6 + view.Face;
			return !version || !cached || !cached->StaticVersions.count(key) || cached->StaticVersions.at(key) != version;
		};
		for (const auto& batch : batches)
		{
			auto& update = pageUpdates[batch.Views.front().Tile.Page];
			update.Exact |= !batch.Latest;
			update.Latest |= batch.Latest;
			for (const auto& view : batch.Views)
			{
				update.Area += UINT64(view.Tile.Side) * view.Tile.Side; // The packer guarantees disjoint tiles.
				update.Updated.emplace(view.Light, view.Face);
				update.KeepPublication |= _localPageSync[view.Tile.Page] == ShadowCrossSync::Latest;
			}
		}
		for (auto& [page, update] : pageUpdates)
		{
			if (!CoversMostOfShadowPage(update.Area, source.Depth->Side) || (update.Exact && update.Latest))
				continue;
			update.Full = true;
			for (UINT i = 0; update.Full && i < source.Lights.size(); ++i)
			{
				const auto& light = source.Lights[i];
				for (UINT face = 0; update.Full && face < LocalShadowFaceCount(light); ++face)
				{
				const auto& cache = source.Cache[i];
				if (!((cache.ReadyFaces | cache.ResidentFaces) & (1u << face)) || light.ShadowTiles[face].Page != page) continue;
					update.Readable.emplace_back(i, face);
					if (update.Updated.count({i, face})) continue;
				const auto order = cache.PublicationOrders[face];
					const auto old = cached ? cached->FinalOrders.find({light.EntityId, face}) :
					    std::map<std::pair<UINT, UINT>, uint64_t>::const_iterator{};
					update.Full = cached && old != cached->FinalOrders.end() && order && old->second == order;
				}
			}
		}
		auto& entry = owners[lane];
		if (!entry) entry = std::make_unique<RemoteShadows>(_submissions, owner, _common, _resources, descriptors);
		auto& remote = *entry;
		remote.SunMask = orderSun && lane == sunLane ? (_sunSync == 3 && !sunAvailable ? _sunRemoteMask : sunUpdate) : 0;
		remote.NearMask = sun.Update & ~_sunRemoteMask;
		const bool latest = lane.Latest;
		const bool publishSun = remote.SunMask && _sunSync == 3;
		const auto publish = [&](bool exact, uint64_t order) -> ShadowPublication&
		{
			auto publication = std::make_shared<ShadowPublication>();
			publication->Owner = owner;
			publication->OriginSlot = _slot;
			publication->Order = order;
			publication->LocalPlacement = _localPlacementEpoch;
			publication->SunPlacement = _sunPlacementEpoch;
			publication->SceneRevision = source.SceneRevision;
			publication->GrassRevision = source.GrassCasterRevision;
			if (exact) frame.ShadowReads.push_back(publication);
			remote.Publications.push_back(publication);
			return *publication;
		};
		auto* heap = remote.Resources.SRV_UAV_Heap.get();
		auto& graph = remote.Plan;
		auto& bindings = remote.Bindings;
		auto& work = remote.Work;
		graph = {};
		graph.Device = owner;
		graph.FrameBoundary = false;
		bindings.clear();
		work.Commands.clear();
		remote.ExactExport = Invalid;
		remote.Lights.clear();
		remote.Count = 0;
		if (lane.Local)
		{
			auto& pass = *remote.Local;
			auto& target = pass._slots.front();
			auto& staticPass = *remote.Static;
			auto& staticTarget = staticPass._slots.front();
			staticTarget.Lights = target.Lights = source.Lights;
			target.Views.clear();
			target.Packed.clear();
			staticTarget.Views.clear();
			staticTarget.Packed.clear();
			pass.PrepareAtlas(target, source.Depth->Side, ownerPages);
			staticPass.PrepareAtlas(staticTarget, source.Depth->Side, ownerPages);
			if (!remote.TileCopy) remote.TileCopy = std::make_unique<LocalShadowTileCopy>(ownerDevice);
			for (const auto& batch : batches)
			{
				for (const auto& view : batch.Views)
				{
					auto tile = view.Tile;
					tile.Page = ownerSlices[tile.Page];
					SceneLocalShadows::PackView(target, source.Cache[view.Light].Faces[view.Face], view.Light, view.Face, tile);
					if (staticDirty(view))
					{
						const auto& cache = statics.Cache[view.Light];
						SceneLocalShadows::PackView(staticTarget, cache.Faces[view.Face], view.Light, view.Face, tile);
						remote.PendingStaticVersions.emplace(view.Light * 6 + view.Face, cache.ContentVersions[view.Face]);
					}
					const auto entity = target.Lights[view.Light].EntityId;
					if (remote.Lights.empty() || remote.Lights.back() != entity) remote.Lights.push_back(entity);
				}
			}
			remote.Count = UINT(target.Views.size());
			pass.Complete(0);
			staticPass.Complete(0);
			if (!target.LightUpload->GetSRV()) target.LightUpload->CreateSRV(heap, heap->GetAvailableIndex(SceneDescriptorStart));
		}
		const auto producer = baselineExports.find(owner);
		std::vector<Access> restore;
		{
			auto mesh = scene.Geometry;
			const auto meshRevision = _resources.GeometryBuffer->GetMeshCacheRevision();
			const bool meshChanged = remote.MeshRevision != meshRevision;
			remote.MeshRevision = meshRevision;
			auto* sourceVertices = mesh.VertexResource;
			auto& baseline = _shadowVertexBaselines[owner];
			if (!baseline || baseline->Source.Get() != sourceVertices)
			{
				baseline = std::make_shared<ShadowVertexBaseline>();
				baseline->CreatedForLocalShadows = lane.Local;
				baseline->Source = sourceVertices;
				const auto shape = CD3DX12_RESOURCE_DESC::Buffer(mesh.Vertices.SizeInBytes);
				baseline->Data = ownerDevice.CreateResource(D3D12_HEAP_TYPE_DEFAULT, shape, D3D12_RESOURCE_STATE_COMMON);
			}
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
				if (index == SceneCommands && !remote.SunMask) continue;
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
			const bool seed = !baseline->Ready && producer == baselineExports.end();
			if (seed) baseline->Version = geometry._vertexDeformationRevision;
			const bool clone = !seed && (remote.VertexSource != sourceVertices || !remote.Mirror.Contains(sourceVertices));
			const auto previousVersion = seed ? 0 : clone ? baseline->Version : remote.VertexDeformationRevision;
			remote.VertexBase = baseline;
			remote.SeedVertices = Invalid;
			remote.Vertices = {previousVersion, geometry._vertexDeformationRevision, 0, 0, 0, seed, clone};
			std::vector<ProbeInputMirror::BufferRange> ranges;
			if (!seed && remote.Vertices.From != remote.Vertices.To)
				for (const auto& [first, end] : geometry.ChangedVertexRanges(remote.Vertices.From))
					ranges.push_back({UINT64(first) * sizeof(Vertex), UINT64(end - first) * sizeof(Vertex)});
			std::vector<ProbeInputMirror::BufferPatch> patches;
			const auto skin = scene.RemoteSkin.find(owner);
			if ((seed || remote.Vertices.From != remote.Vertices.To) &&
			    skin != scene.RemoteSkin.end() && !skin->second.Ranges.empty())
			{
				remote.SkinInput = skin->second.Output;
				for (const auto& range : skin->second.Ranges)
				{
					const auto bytes = range.Work.VertexBytes();
					patches.push_back({remote.SkinInput->Get(), UINT64(range.FirstVertex) * sizeof(Vertex),
					    UINT64(range.Work.OutputVertex) * sizeof(Vertex), bytes});
					remote.Vertices.LocalBytes += bytes;
				}
			}
			mesh.VertexResource = remote.Mirror.Mirror(primary, primaryBindings, mesh.VertexResource,
			    SceneVertices, seed, std::move(patches), std::move(ranges), clone ? baseline->Data.Get() : nullptr);
			remote.Vertices.TransferBytes = remote.Mirror.PendingTransferBytes(sourceVertices);
			remote.Vertices.BaselineBytes = remote.Mirror.PendingBaselineBytes(sourceVertices);
			mesh.IndexResource = mirror(SceneIndices, meshChanged);
			mesh.Vertices.BufferLocation = mesh.VertexResource->GetGPUVirtualAddress();
			mesh.Indices.BufferLocation = mesh.IndexResource->GetGPUVirtualAddress();
			inputs[3] = bind(mesh.VertexResource);
			inputs[4] = bind(mesh.IndexResource);
			if (seed)
			{
				auto* target = baseline->Data.Get();
				const Id targetId = bind(target);
				remote.Vertices.BaselineBytes = mesh.Vertices.SizeInBytes;
				remote.SeedVertices = work.Add(graph, "seed shadow owner vertex baseline",
				    {{inputs[3], Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE},
				     {targetId, Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST}},
				    [target, mesh](auto* cmd)
				    { cmd->CopyBufferRegion(target, 0, mesh.VertexResource, 0, mesh.Vertices.SizeInBytes); },
				    {}, Queue::Copy);
			}
			const Id cameraSource = NativeWork::ResourceFor(primary, primaryBindings, cameraBuffer, "shadow camera input");
			auto* localCamera = mirror(cameraSource, true);
			const Id cameraId = bind(localCamera);
			const auto cameraAddress = localCamera->GetGPUVirtualAddress() +
			    UINT64(_common.ActiveCameraCBufferIndex) * camera.GetElementSize();
			const bool materialsChanged = remote.TextureRevision != _resources.TextureRevision;
			remote.TextureRevision = _resources.TextureRevision;
			std::vector<Access> materialReads;
			for (auto* texture : textures)
			{
				auto& resource = *texture->GetResource();
				auto* native = resource.D3DResource.Get();
				Id id = primary.FindBinding(primaryBindings, native);
				if (id == Invalid)
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
			if (lane.Local)
			{
				auto& pass = *remote.Local;
				auto& target = pass._slots.front();
				auto& staticPass = *remote.Static;
				auto& staticTarget = staticPass._slots.front();
				const std::array<Id, 5> localInputs{inputs[0], inputs[1], inputs[2], inputs[3], inputs[4]};
				staticPass.Append(graph, bindings, 0, descriptors, mesh, *target.LightUpload,
				    vegetation, &localInputs, materialReads);
				addWork(staticTarget.Work);
				std::vector<LocalShadowTileCopy::Region> restoreDepth;
				for (const auto& view : target.Views)
					restoreDepth.push_back({view.Tile, view.Tile, target.Depth->Dsvs[view.Tile.Page]->CPUHandle});
				remote.TileCopy->Draw(graph, bindings, work,
				    staticTarget.Depth->Texture.GetResource()->D3DResource.Get(), target.Depth->Texture.GetResource()->D3DResource.Get(),
				    heap->GetHeap().Get(), staticTarget.Depth->Texture.GetSRV()->GPUHandle,
				    std::move(restoreDepth), "restore owner static shadow tiles");
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
		}
		if (remote.Vertices.Seed) ++remote.SeedVertices; // The scene import was prepended.
		remote.Export = remote.Mirror.AppendExport(primary, primaryBindings, _work, restore.empty() ? Queue::Copy : Queue::Direct);
		if (remote.Vertices.Seed) baselineExports.emplace(owner, remote.Export);
		else if (remote.Vertices.Clone && producer != baselineExports.end())
			primary.Operations[remote.Export].Dependencies.push_back(producer->second);
		Id restored = Invalid;
		if (!restore.empty())
			restored = _work.Add(primary, "restore GPU0 shadow material textures", UniqueLightingAccesses(std::move(restore)),
			                     [](auto*) {}, {remote.Export});
		scene.ShadowInputCopies.emplace_back(remote.Export, restored);
		const auto waitForOwner = [&](Id imported)
		{
			if (remote.Import == Invalid) remote.Import = imported;
			else primary.Operations[imported].Dependencies.push_back(remote.Import);
		};
		const auto acceptExact = [&](const SceneLocalShadows::View& view, uint64_t order)
		{
			source.RemoteViews.push_back(view);
			_localShadows->MarkDepthWritten(_slot, view.Light, view.Face);
			source.Cache[view.Light].RemoteFaces |= 1u << view.Face;
			source.Cache[view.Light].PublicationOrders[view.Face] = order;
		};
		const auto exportPage = [&](UINT page, const PageUpdate& update)
		{
			auto& target = *remote.Local->_slots.front().Depth;
			auto transfer = std::make_shared<LocalShadowPageTransfer>(*_submissions, owner, target.Side, update.KeepPublication);
			const auto exported = transfer->Export(graph, bindings, work,
			    target.Texture.GetResource()->D3DResource.Get(), ownerSlices[page]);
			const auto order = ++_shadowOrder;
			ShadowPublication* publication = nullptr;
			if (update.KeepPublication)
			{
				publication = &publish(update.Exact, order);
				publication->LocalPage = page;
				publication->Page = transfer;
			}
			for (const auto& [lightIndex, face] : update.Readable)
			{
				const auto& light = source.Lights[lightIndex];
				const auto& cache = source.Cache[lightIndex];
				const bool written = update.Updated.count({lightIndex, face}) != 0;
				if (written) remote.FinalOrders[{light.EntityId, face}] = order;
				if (!publication) continue;
				auto& entry = publication->Lights[light.EntityId];
				entry.Metadata = light;
				entry.Generation = cache.Generation;
				entry.PageOrders[face] = written ? order : cache.PublicationOrders[face];
				if (written) entry.Faces.emplace_back(0, face);
			}
			if (update.Exact)
			{
				remote.ExactExport = exported;
				waitForOwner(transfer->Import(primary, primaryBindings, _work, localAtlas(), page));
				for (const auto& batch : batches)
					if (batch.Views.front().Tile.Page == page)
						for (const auto& view : batch.Views) acceptExact(view, order);
			}
			remote.PageTransfers.push_back(std::move(transfer));
		};
		const auto exportTiles = [&](const Batch& batch)
		{
			const auto& batchViews = batch.Views;
			const UINT side = batchViews.front().Tile.Side;
			const UINT count = UINT(batchViews.size());
			auto& target = *remote.Local->_slots.front().Depth;
			auto transfer = std::make_shared<LocalShadowTileTransfer>(*_submissions, owner, side, count, false);
			std::vector<GDX12LocalShadowTile> tiles;
			for (const auto& view : batchViews)
			{
				auto tile = view.Tile;
				tile.Page = ownerSlices[tile.Page];
				tiles.push_back(tile);
			}
			const auto exported = transfer->Export(graph, bindings, work, *remote.TileCopy,
			    target.Texture.GetResource()->D3DResource.Get(), std::move(tiles));
			ShadowPublication* publication = nullptr;
			const auto order = ++_shadowOrder;
			for (UINT index = 0; index < count; ++index)
			{
				const auto& view = batchViews[index];
				const auto& light = source.Lights[view.Light];
				remote.FinalOrders[{light.EntityId, view.Face}] = order;
				if (_localPageSync[view.Tile.Page] != ShadowCrossSync::Latest) continue;
				if (!publication)
				{
					publication = &publish(!batch.Latest, order);
					publication->Local = transfer;
				}
				auto& entry = publication->Lights[light.EntityId];
				entry.Metadata = light;
				entry.Generation = source.Cache[view.Light].Generation;
				entry.Faces.emplace_back(index, view.Face);
			}
			if (!batch.Latest)
			{
				remote.ExactExport = exported;
				waitForOwner(transfer->Stage(primary, primaryBindings, _work));
				if (publication) publication->LocalStaged = true;
				std::vector<LocalShadowTileCopy::Region> regions;
				for (UINT index = 0; index < count; ++index) regions.push_back(makeRegion(index, batchViews[index].Tile));
				transfer->Publish(primary, primaryBindings, _work, MainShadowTileCopy(), localAtlas(), std::move(regions));
				for (const auto& view : batchViews) acceptExact(view, order);
			}
			remote.Transfers.push_back(std::move(transfer));
		};
		// Exact exports share the DIRECT timeline. Submit them before optional Latest traffic.
		for (const bool latestResult : {false, true})
		{
			for (const auto& [page, update] : pageUpdates)
				if (update.Full && update.Latest == latestResult) exportPage(page, update);
			for (const auto& batch : batches)
				if (batch.Latest == latestResult && !pageUpdates.at(batch.Views.front().Tile.Page).Full) exportTiles(batch);
		}
		if (remote.SunMask)
		{
			if (!remote.SunTransfer || remote.SunTransfer.use_count() > 1)
				remote.SunTransfer = std::make_shared<SunShadowTileTransfer>(*_submissions, owner,
				    *_resources.SRV_UAV_Heap, *heap, remote.SunTransfer.get());
			remote.SunTransfer->Prepare(_resources.ShadowQuality.SunResolution, remote.SunMask);
			remote.SunTransfer->Export(graph, bindings, work, *remote.Sun->Output(0));
			if (publishSun)
			{
				auto& publication = publish(!latest, ++_shadowOrder);
				publication.Sun = remote.SunTransfer;
				publication.SunResolution = _resources.ShadowQuality.SunResolution;
				publication.SunMask = remote.SunMask;
				publication.SunRevision = _resources.Lighting.SunShadowRevision;
				publication.SunSettings = sunSettings;
			}
			if (!latest)
			{
				const Id imported = remote.SunTransfer->Import(primary, primaryBindings, _work, sun.Atlas);
				waitForOwner(imported);
				sun.Settings = sunSettings;
				sun.Revision = _resources.Lighting.SunShadowRevision;
				sun.DirtyCascades &= ~remote.SunMask;
				if (publishSun) remote.Publications.front()->SunStaged = true;
				for (UINT tile = 0; tile < frame.SunShadowOrders.size(); ++tile)
					if (remote.SunMask & (1u << tile))
						frame.SunShadowOrders[tile] = publishSun ? remote.Publications.front()->Order : 0;
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
		const Id seed = remote.SeedVertices == Invalid ? Invalid : packet(schedule, remote.SeedVertices);
		const Id exactExport = remote.ExactExport == Invalid ? Invalid : packet(schedule, remote.ExactExport);
		// Only optional Latest exports can follow the final local Exact export in this plan.
		const Id signalAfter = exactExport != Invalid && remote.ExactExport + 1 < remote.Plan.Operations.size() ?
		    exactExport : Invalid;
		remote.GPU.Begin(0);
		remote.GPU.Reserve(schedule);
		exchanges.push_back({packet(primary, remote.Export), remote.Import,
		    [&remote, schedule = std::move(schedule), input, seed, exactExport, signalAfter](CompletionToken source)
		    {
			    std::vector<PacketWait> waits{{input, source}};
			    if (remote.Vertices.Clone)
			    {
				    if (!remote.VertexBase->Ready) throw std::logic_error("Shadow vertex baseline was not submitted");
				    waits.push_back({input, remote.VertexBase->Ready});
			    }
			    remote.GPU.Execute(schedule, remote.Bindings, [&remote](Id op, UINT, ID3D12GraphicsCommandList* cmd)
			    {
				    if (!remote.Work.Record(op, cmd)) throw std::logic_error("Missing owner shadow work");
			    }, nullptr, nullptr, nullptr, waits, {}, signalAfter);
			    remote.Produced = remote.GPU.Completions().back();
			    remote.ExactProduced = exactExport == Invalid ? remote.Produced : remote.GPU.Completions().at(exactExport);
			    for (const auto& [face, version] : remote.PendingStaticVersions)
				    remote.StaticVersions[face] = version;
			    remote.PendingStaticVersions.clear();
			    if (seed != Invalid) remote.VertexBase->Ready = remote.GPU.Completions().at(seed);
			    remote.VertexSource = remote.VertexBase->Source.Get();
			    remote.VertexDeformationRevision = remote.Vertices.To;
			    return remote.ExactProduced;
		    }});
	}
}

void SceneLighting::ShadowImportsSubmitted(const std::vector<CompletionToken>& completions)
{
	if (!completions.empty()) _frames.at(_slot).ShadowConsumer = completions.back();
	for (auto& [owner, entry] : _frames.at(_slot).ShadowOwners)
	{
		auto& remote = *entry;
		if (remote.Import == Invalid) continue;
		remote.Imported = completions.at(remote.Import);
		remote.Consumer = completions.back();
	}
}

void SceneLighting::RetireShadowSkinInputs(UINT slot)
{
	for (auto& [lane, entry] : _frames.at(slot).ShadowOwners)
	{
		auto& remote = *entry;
		if (!remote.Produced || !_submissions->Complete(remote.Produced)) continue;
		remote.Work.Commands.clear(); // Retire captured upload buffers with their owner fence.
		remote.Transfers.clear();
		remote.PageTransfers.clear();
		const auto& update = remote.Vertices;
		auto& reported = update.Seed || update.Clone ? remote.ReportedVertexBaseline : remote.ReportedVertexDelta;
		if (!reported && (update.TransferBytes || update.LocalBytes || update.BaselineBytes))
		{
			Log::Info("RenderGraph", "Shadow vertex mirror ", update.Seed ? "baseline" : update.Clone ? "clone" : "delta",
			    " owner=", _submissions->AdapterLabel(lane.Owner), " slot=", slot, " latest=", lane.Latest,
			    " from=", update.From, " version=", update.To, " crossAdapterBytes=", update.TransferBytes,
			    " ownerLocalBytes=", update.LocalBytes, " baselineLocalBytes=", update.BaselineBytes,
			    " shadowFence=", remote.Produced.Value,
			    " frameFence=", remote.Consumer.Value);
			reported = true;
		}
		if (remote.SkinInput && _reportedShadowSkinOwners.insert(lane.Owner).second)
			Log::Info("RenderGraph", "Shadow owner reused local Exact skinning GPU=", _submissions->AdapterLabel(lane.Owner),
			    " slot=", slot, " vertexBytes=", update.LocalBytes, " shadowFence=", remote.Produced.Value);
		remote.SkinInput.reset();
		remote.VertexBase.reset();
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
		         for (UINT i = 0; i < 18; ++i)
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
	    [this] { return _probePreview->Depth(); }, [this] { return _probePreview->Motion(); });
	auto* hdr = _transparency->Append(graph, bindings, geometry, _slot, opaqueHDR, environment);
	_hdrContinuation(graph, bindings, hdr);
}
} // namespace BistroRender

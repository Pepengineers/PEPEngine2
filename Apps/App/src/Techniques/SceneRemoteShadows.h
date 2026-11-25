#pragma once

#include "SceneLighting.h"
#include "ProbeInputMirror.h"
#include "LocalShadowTileCopy.h"
#include "SunShadowTileTransfer.h"

namespace BistroRender
{
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
} // namespace BistroRender

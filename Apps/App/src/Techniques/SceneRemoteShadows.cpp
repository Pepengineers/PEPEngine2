#include "SceneRemoteShadows.h"
#include "ShadowGeometry.h"
#include "Engine.Core/Logging/Logger.h"
#include <algorithm>
#include <cstring>
#include <set>

namespace BistroRender
{
namespace
{
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
} // namespace

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

struct SceneLighting::RemoteShadowPlan
{
	SceneLighting& lighting;
	Graph& primary;
	std::vector<ID3D12Resource*>& primaryBindings;
	SceneGeometry& geometry;
	Frame& frame = lighting._frames.at(lighting._slot);
	SceneLocalShadows::Slot& source = lighting._localShadows->_slots.at(lighting._slot);
	SceneSunShadows::Slot& sun = lighting._sunShadows->_slots.at(lighting._slot);
	const GDX12ShadowConstants sunSettings = lighting._resources.ShadowSettings;

	struct FaceAvailability
	{
		UINT Available = 0, Current = 0;
	};
	struct Batch
	{
		std::vector<SceneLocalShadows::View> Views;
		bool Latest = false;
	};
	struct PageUpdate
	{
		UINT64 Area = 0;
		bool Exact = false, Latest = false, Full = false, KeepPublication = false;
		std::set<std::pair<UINT, UINT>> Updated;
		std::vector<std::pair<UINT, UINT>> Readable;
	};
	using LaneBatches = std::map<ShadowLane, std::vector<Batch>>;
	using PageUpdates = std::map<UINT, PageUpdate>;

	struct MeshInputs
	{
		GDX12GeometryBuffer::RecordingBinding Mesh;
		std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 4> Descriptors{};
		std::array<Id, 6> Resources{};
	};

	ID3D12Resource* LocalAtlas() const
	{
		return source.Depth->Texture.GetResource()->D3DResource.Get();
	}

	LocalShadowTileCopy::Region Region(UINT packed, const GDX12LocalShadowTile& tile) const
	{
		return LocalShadowTileCopy::Region{{packed, 0, 0, tile.Side}, tile, source.Depth->Dsvs[tile.Page]->CPUHandle};
	}

	bool CompatibleSun(const ShadowPublication& publication) const
	{
		return lighting._sunSync == 3 && publication.Sun && publication.Owner == lighting._sunOwner &&
		       publication.SunPlacement == lighting._sunPlacementEpoch &&
		       publication.SunResolution == lighting._resources.ShadowQuality.SunResolution &&
		       std::memcmp(&publication.SunSettings, &sunSettings, sizeof(sunSettings)) == 0;
	}

	const ShadowPublication::LocalLight* CompatibleLocalDepth(const ShadowPublication& publication, UINT index, UINT face) const
	{
		const auto& light = source.Lights[index];
		const auto page = light.ShadowTiles[face].Page;
		const auto found = publication.Lights.find(light.EntityId);
		return page < lighting._localPageOwners.size() && lighting._localPageOwners[page] == publication.Owner &&
		       publication.LocalPlacement == lighting._localPlacementEpoch && found != publication.Lights.end() &&
		       found->second.Metadata.ShadowTiles[face].Side == light.ShadowTiles[face].Side &&
		       found->second.Generation == source.Cache[index].Generation &&
		       CompatibleLocalShadow(found->second.Metadata, light) ? &found->second : nullptr;
	}

	bool CompatibleLocal(const ShadowPublication& publication, UINT index, UINT face) const
	{
		return (source.Cache[index].LatestFaces & (1u << face)) && CompatibleLocalDepth(publication, index, face);
	}

	bool CurrentCasters(const ShadowPublication& publication) const
	{
		return publication.SceneRevision == source.SceneRevision && publication.GrassRevision == source.GrassCasterRevision;
	}

	void CollectPublications()
	{
		const auto sunAtlas = sun.Atlas.GetResource()->D3DResource;
		// Begin has retired this primary slot. Other slots retain the publications they read.
		const UINT completed = frame.CrossSlotShadows & ~lighting._reportedCrossSlotShadows;
		if (completed && frame.ShadowConsumer && lighting._submissions->Complete(frame.ShadowConsumer))
		{
			Log::Info("RenderGraph", "GPU0 completed cross-slot shadow publication consumer fence=", frame.ShadowConsumer.Value,
			          " slot=", lighting._slot, " local=", bool(completed & 1u), " CSM=", bool(completed & 2u));
			lighting._reportedCrossSlotShadows |= completed;
		}
		frame.CrossSlotShadows = 0;
		frame.ShadowReads.clear();
		if (frame.LatestSunAtlas.Get() != sunAtlas.Get())
			frame.SunShadowOrders.fill(0);
		frame.LatestSunAtlas = sunAtlas;

		for (auto it = lighting._latestLocalShadows.begin(); it != lighting._latestLocalShadows.end();)
		{
			const auto light = std::lower_bound(source.Lights.begin(), source.Lights.end(), it->first,
			    [](const auto& item, UINT id) { return item.EntityId < id; });
			if (light == source.Lights.end() || light->EntityId != it->first || !source.Cache[light - source.Lights.begin()].LatestFaces)
				it = lighting._latestLocalShadows.erase(it);
			else
			{
				for (UINT face = 0; face < it->second.size(); ++face)
					if (it->second[face] && !CompatibleLocal(*it->second[face], UINT(light - source.Lights.begin()), face))
						it->second[face].reset();
				++it;
			}
		}
		for (auto& cascade : lighting._latestSunShadows)
			if (cascade && cascade->SunPlacement != lighting._sunPlacementEpoch) cascade.reset();

		for (auto& candidateFrame : lighting._frames)
			for (auto& [lane, remote] : candidateFrame.ShadowOwners)
			{
				if (remote->Publications.empty() || !remote->Produced || !lighting._submissions->Complete(remote->Produced)) continue;
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
							if (!CompatibleLocal(*publication, index, face)) continue;
							auto& selected = lighting._latestLocalShadows[entity][face];
							if (!selected || !CompatibleLocal(*selected, index, face) || selected->Order < publication->Order)
								selected = publication;
						}
					}
					if (CompatibleSun(*publication))
						for (UINT tile = 0; tile < lighting._latestSunShadows.size(); ++tile)
						{
							if (!(publication->SunMask & (1u << tile))) continue;
							auto& selected = lighting._latestSunShadows[tile];
							if (!selected || !CompatibleSun(*selected) || selected->Order < publication->Order)
								selected = publication;
						}
				}
				remote->Publications.clear();
			}

		for (auto& [lane, remote] : frame.ShadowOwners)
		{
			remote->Export = remote->Import = Invalid;
			if (!remote->Reported && remote->Consumer && lighting._submissions->Complete(remote->Consumer))
			{
				lighting._submissions->LogFirstImport("local shadow depth", remote->ExactProduced, remote->Imported);
				for (const UINT light : remote->Lights)
					Log::Info("RenderGraph", "GPU0 completed local shadow consumer fence=", remote->Consumer.Value,
					          " entity=", light, " slices=", remote->Count, " owner=", lighting._submissions->AdapterLabel(lane.Owner));
				if (remote->SunMask)
					Log::Info("RenderGraph", "GPU0 completed CSM consumer fence=", remote->Consumer.Value,
					          " GPU0 cascade mask=", remote->NearMask, " imported cascade mask=", remote->SunMask,
					          " owner=", lighting._submissions->AdapterLabel(lane.Owner));
				remote->Reported = true;
			}
		}
	}

	std::vector<FaceAvailability> ImportLocalFaces()
	{
		std::vector<FaceAvailability> facesByLight(source.Lights.size());
		std::map<ShadowPublication*, std::vector<LocalShadowTileCopy::Region>> imports;
		for (UINT i = 0; i < source.Lights.size(); ++i)
		{
			const auto& light = source.Lights[i];
			if (!source.Cache[i].LatestFaces || light.ShadowFirstSlice == UINT_MAX) continue;
			const auto found = lighting._latestLocalShadows.find(light.EntityId);
			if (found == lighting._latestLocalShadows.end()) continue;
			const auto& faces = found->second;
			const UINT needed = light._padding[2];
			for (UINT face = 0; face < faces.size(); ++face)
			{
				if (!(needed & (1u << face)) || !faces[face] || !CompatibleLocal(*faces[face], i, face)) continue;
				facesByLight[i].Available |= 1u << face;
				const auto& publication = faces[face];
				if (CurrentCasters(*publication)) facesByLight[i].Current |= 1u << face;
				auto& order = source.Cache[i].PublicationOrders[face];
				if (order >= publication->Order) continue;
				const auto& tiles = publication->Lights.at(light.EntityId).Faces;
				const auto tile = std::find_if(tiles.begin(), tiles.end(), [=](const auto& entry) { return entry.second == face; });
				if (tile == tiles.end()) throw std::logic_error("Missing published local shadow face");
				auto region = Region(tile->first, light.ShadowTiles[face]);
				if (publication->Page)
				{
					region.From = publication->Lights.at(light.EntityId).Metadata.ShadowTiles[face];
					region.From.Page = 0;
				}
				imports[publication.get()].push_back(region);
				lighting._localShadows->MarkDepthWritten(lighting._slot, i, face);
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
						const auto* old = CompatibleLocalDepth(*publication, i, face);
						coherent = old && old->Metadata.ShadowTiles[face] == tile && old->PageOrders[face] &&
						    old->PageOrders[face] == cache.PublicationOrders[face];
					}
				}
				if (coherent)
					publication->Page->Import(primary, primaryBindings, lighting._work, LocalAtlas(), page);
				else
				{
					publication->Page->Publish(primary, primaryBindings, lighting._work, lighting.MainShadowTileCopy(), LocalAtlas(), std::move(regions));
				}
			}
			else
			{
				if (!publication->LocalStaged)
				{
					publication->Local->Stage(primary, primaryBindings, lighting._work);
					publication->LocalStaged = true;
				}
				publication->Local->Publish(primary, primaryBindings, lighting._work, lighting.MainShadowTileCopy(), LocalAtlas(), std::move(regions));
			}
			if (publication->OriginSlot != lighting._slot) frame.CrossSlotShadows |= 1u;
		}
		return facesByLight;
	}

	bool ImportSunCascades()
	{
		UINT sunCovered = 0;
		for (UINT tile = 0; tile < lighting._latestSunShadows.size(); ++tile)
			if (lighting._latestSunShadows[tile] && CompatibleSun(*lighting._latestSunShadows[tile])) sunCovered |= 1u << tile;
		if (!lighting._sunRemoteMask || (sunCovered & lighting._sunRemoteMask) != lighting._sunRemoteMask) return false;

		std::map<ShadowPublication*, UINT> sunImports;
		for (UINT tile = 0; tile < lighting._latestSunShadows.size(); ++tile)
		{
			const UINT bit = 1u << tile;
			if (!(lighting._sunRemoteMask & bit)) continue;
			const auto& publication = lighting._latestSunShadows[tile];
			if (frame.SunShadowOrders[tile] != publication->Order)
			{
				sunImports[publication.get()] |= bit;
				frame.SunShadowOrders[tile] = publication->Order;
				frame.ShadowReads.push_back(publication);
			}
			if (CurrentCasters(*publication) && publication->SunRevision == lighting._resources.Lighting.SunShadowRevision &&
			    !(lighting._resources.Lighting.SunShadowFlags & ShadowEveryFrame))
			{
				sun.DirtyCascades &= ~bit;
				sun.Update &= ~bit;
			}
		}
		for (const auto& [publication, mask] : sunImports)
		{
			if (!publication->SunStaged)
			{
				publication->Sun->Stage(primary, primaryBindings, lighting._work);
				publication->SunStaged = true;
			}
			publication->Sun->Publish(primary, primaryBindings, lighting._work, sun.Atlas, mask);
			if (publication->OriginSlot != lighting._slot) frame.CrossSlotShadows |= 2u;
		}
		return true;
	}

	LaneBatches SelectBatches(const std::vector<FaceAvailability>& faces, bool sunAvailable)
	{
		LaneBatches views;
		const auto busy = [&](Id owner, bool local)
		{
			const auto found = frame.ShadowOwners.find({owner, !local, local});
			return found != frame.ShadowOwners.end() && !found->second->Publications.empty();
		};
		source.Views.erase(std::remove_if(source.Views.begin(), source.Views.end(), [&](const auto& view)
		{
			const Id owner = lighting._localPageOwners[view.Tile.Page];
			if (!owner) return false;
			const bool latest = (faces[view.Light].Available & (1u << view.Face)) != 0;
			if (latest && (faces[view.Light].Current & (1u << view.Face)) && !(source.Lights[view.Light].ShadowFlags & ShadowEveryFrame))
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
			for (auto it = lighting._shadowVertexBaselines.begin(); it != lighting._shadowVertexBaselines.end();)
				if (it->second->CreatedForLocalShadows && it->second.use_count() == 1) it = lighting._shadowVertexBaselines.erase(it);
				else ++it;
		const UINT sunUpdate = sun.Update & lighting._sunRemoteMask;
		const ShadowLane sunLane{lighting._sunOwner, sunAvailable, false};
		const bool orderSun = sunUpdate && (!sunAvailable || !busy(lighting._sunOwner, false));
		if (orderSun) views.try_emplace(sunLane);
		return views;
	}

	std::vector<GDX12Texture*> CollectMaskedTextures(std::unordered_set<ID3D12Resource*>& live) const
	{
		auto& materials = *lighting._resources.FrameConstants.at(lighting._slot)->MaterialCache;
		std::set<UINT> maskedTextures;
		for (UINT i = 0; i < materials.GetElementCount(); ++i)
		{
			const auto& material = materials.GetCPUData()[i];
			if (material.RenderLayer == UINT(MaterialType::Masked) && !material.ReflectionProbePreview)
				maskedTextures.insert(material.DiffuseIndex);
		}
		std::vector<GDX12Texture*> textures;
		for (const auto& [name, texture] : lighting._resources.Textures)
		{
			const UINT index = texture->GetSRV()->HeapIndex;
			if (index < Texture2D_StartIndex || !maskedTextures.count(index - Texture2D_StartIndex)) continue;
			textures.push_back(texture.get());
			live.insert(texture->GetResource()->D3DResource.Get());
		}
		return textures;
	}

	void FallbackToPrimary(const std::vector<Batch>& batches)
	{
		auto& statics = lighting._staticShadows->_slots.at(lighting._slot);
		for (const auto& batch : batches)
		{
			if (batch.Latest) continue;
			for (const auto& view : batch.Views)
			{
				lighting._effectiveLocalPageOwners[view.Tile.Page] = 0;
				auto& cache = source.Cache[view.Light];
				cache.LocalFallback |= 1u << view.Face;
				source.Views.push_back(view);
				cache.PublicationOrders[view.Face] = ++lighting._shadowOrder;
				if (std::none_of(statics.Views.begin(), statics.Views.end(), [&](const auto& dirty) {
				        return dirty.Light == view.Light && dirty.Face == view.Face;
				    }))
					SceneLocalShadows::PackView(statics, statics.Cache[view.Light].Faces[view.Face],
					    view.Light, view.Face, view.Tile);
			}
		}
	}

	PageUpdates PlanPages(const std::vector<Batch>& batches, const RemoteShadows* cached) const
	{
		PageUpdates pageUpdates;
		for (const auto& batch : batches)
		{
			auto& update = pageUpdates[batch.Views.front().Tile.Page];
			update.Exact |= !batch.Latest;
			update.Latest |= batch.Latest;
			for (const auto& view : batch.Views)
			{
				update.Area += UINT64(view.Tile.Side) * view.Tile.Side; // The packer guarantees disjoint tiles.
				update.Updated.emplace(view.Light, view.Face);
				update.KeepPublication |= lighting._localPageSync[view.Tile.Page] == ShadowCrossSync::Latest;
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
					if (!cached || !order)
					{
						update.Full = false;
						break;
					}
					const auto old = cached->FinalOrders.find({light.EntityId, face});
					update.Full = old != cached->FinalOrders.end() && old->second == order;
				}
			}
		}
		return pageUpdates;
	}

	void PrepareAtlases(RemoteShadows& remote, const std::vector<Batch>& batches,
	                   const std::vector<UINT>& ownerSlices, UINT ownerPages)
	{
		auto& statics = lighting._staticShadows->_slots.at(lighting._slot);
		auto* heap = remote.Resources.SRV_UAV_Heap.get();
		const auto staticDirty = [&](const SceneLocalShadows::View& view)
		{
			const auto version = statics.Cache[view.Light].ContentVersions[view.Face];
			const auto key = view.Light * 6 + view.Face;
			const auto found = remote.StaticVersions.find(key);
			return !version || found == remote.StaticVersions.end() || found->second != version;
		};
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
		if (!remote.TileCopy) remote.TileCopy = std::make_unique<LocalShadowTileCopy>(*remote.Resources.Device);
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

	ShadowPublication& Publish(RemoteShadows& remote, bool exact, uint64_t order)
	{
		auto publication = std::make_shared<ShadowPublication>();
		publication->Owner = remote.Plan.Device;
		publication->OriginSlot = lighting._slot;
		publication->Order = order;
		publication->LocalPlacement = lighting._localPlacementEpoch;
		publication->SunPlacement = lighting._sunPlacementEpoch;
		publication->SceneRevision = source.SceneRevision;
		publication->GrassRevision = source.GrassCasterRevision;
		if (exact) frame.ShadowReads.push_back(publication);
		remote.Publications.push_back(publication);
		return *publication;
	}

	MeshInputs MirrorMesh(const ShadowLane& lane, RemoteShadows& remote, bool baselinePlanned,
	                      const std::unordered_set<ID3D12Resource*>& live)
	{
		auto& scene = geometry._slots.at(lighting._slot);
		const Id owner = lane.Owner;
		auto& ownerDevice = lighting._submissions->Device(owner);
		auto* heap = remote.Resources.SRV_UAV_Heap.get();
		auto& graph = remote.Plan;
		auto& bindings = remote.Bindings;
		auto& work = remote.Work;
		MeshInputs result{scene.Geometry};
		auto& mesh = result.Mesh;
		auto& descriptors = result.Descriptors;
		auto& inputs = result.Resources;
		const auto meshRevision = lighting._resources.GeometryBuffer->GetMeshCacheRevision();
		const bool meshChanged = remote.MeshRevision != meshRevision;
		remote.MeshRevision = meshRevision;
		auto* sourceVertices = mesh.VertexResource;
		auto& baseline = lighting._shadowVertexBaselines[owner];
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
		const bool seed = !baseline->Ready && !baselinePlanned;
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
		return result;
	}

	void AppendOwnerScene(const ShadowLane& lane, RemoteShadows& remote, const std::unordered_set<ID3D12Resource*>& live,
	                      const std::vector<GDX12Texture*>& textures, std::map<Id, Id>& baselineExports)
	{
		const Id owner = lane.Owner;
		auto& graph = remote.Plan;
		auto& bindings = remote.Bindings;
		auto& work = remote.Work;
		auto* heap = remote.Resources.SRV_UAV_Heap.get();
		auto& scene = geometry._slots.at(lighting._slot);
		auto& camera = *lighting._resources.FrameConstants.at(lighting._slot)->CameraCB;
		auto* cameraBuffer = camera.GetResource().D3DResource.Get();
		const auto producer = baselineExports.find(owner);
		std::vector<Access> restore;
		auto meshInputs = MirrorMesh(lane, remote, producer != baselineExports.end(), live);
		const auto& mesh = meshInputs.Mesh;
		const auto& inputs = meshInputs.Resources;
		const auto& descriptors = meshInputs.Descriptors;
		const auto mirror = [&](Id id, bool refresh)
		{ return remote.Mirror.Mirror(primary, primaryBindings, primaryBindings.at(id), id, refresh); };
		const auto bind = [&](ID3D12Resource* resource)
		{ return NativeWork::ResourceFor(graph, bindings, resource, "shadow owner input"); };
		const Id cameraSource = NativeWork::ResourceFor(primary, primaryBindings, cameraBuffer, "shadow camera input");
		auto* localCamera = mirror(cameraSource, true);
		const Id cameraId = bind(localCamera);
		const auto cameraAddress = localCamera->GetGPUVirtualAddress() +
		    UINT64(lighting._common.ActiveCameraCBufferIndex) * camera.GetElementSize();
		const bool materialsChanged = remote.TextureRevision != lighting._resources.TextureRevision;
		remote.TextureRevision = lighting._resources.TextureRevision;
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
			grass.View.FrameSlot = lighting._slot;
			grass.Camera = cameraId;
			grass.View.CameraConstants = cameraAddress;
			grass.AdditionalReads = materialReads;
			geometry.AppendVegetationView(primary, primaryBindings, remote.Mirror, graph, bindings, work,
			    remote.Vegetation, lighting._slot, grass, remote.SceneViews[SceneMaterials], inputs[1], Invalid, 0, {});
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
			if (!remote.Sun) remote.Sun = std::make_unique<SceneSunShadows>(remote.Resources, lighting._common);
			auto& shadow = *remote.Sun;
			remote.Resources.ShadowQuality.SunResolution = lighting._resources.ShadowQuality.SunResolution;
			remote.Resources.ShadowSettings = lighting._resources.ShadowSettings;
			remote.Resources.FrameConstants.front()->ShadowCB->CopyData(0, lighting._resources.ShadowSettings);
			shadow._slots.front().Update = remote.SunMask;
			shadow.Append(graph, bindings, 0, descriptors, scene.Draws, mesh, vegetation, remote.SunMask,
			              &inputs, materialReads);
			addWork(shadow._slots.front().Work);
		}
		std::sort(work.Commands.begin(), work.Commands.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
		remote.Input = remote.Mirror.AppendImport(graph, bindings, work, true);
		if (remote.Vertices.Seed) ++remote.SeedVertices; // The scene import was prepended.
		remote.Export = remote.Mirror.AppendExport(primary, primaryBindings, lighting._work, restore.empty() ? Queue::Copy : Queue::Direct);
		if (remote.Vertices.Seed) baselineExports.emplace(owner, remote.Export);
		else if (remote.Vertices.Clone && producer != baselineExports.end())
			primary.Operations[remote.Export].Dependencies.push_back(producer->second);
		Id restored = Invalid;
		if (!restore.empty())
			restored = lighting._work.Add(primary, "restore GPU0 shadow material textures", UniqueLightingAccesses(std::move(restore)),
			                     [](auto*) {}, {remote.Export});
		scene.ShadowInputCopies.emplace_back(remote.Export, restored);
	}

	void WaitForOwner(RemoteShadows& remote, Id imported)
	{
		if (remote.Import == Invalid) remote.Import = imported;
		else primary.Operations[imported].Dependencies.push_back(remote.Import);
	}

	void AcceptExact(const SceneLocalShadows::View& view, uint64_t order)
	{
		source.RemoteViews.push_back(view);
		lighting._localShadows->MarkDepthWritten(lighting._slot, view.Light, view.Face);
		source.Cache[view.Light].RemoteFaces |= 1u << view.Face;
		source.Cache[view.Light].PublicationOrders[view.Face] = order;
	}

	void ExportPage(RemoteShadows& remote, UINT page, const PageUpdate& update,
	                const std::vector<Batch>& batches, const std::vector<UINT>& ownerSlices)
	{
		auto& target = *remote.Local->_slots.front().Depth;
		auto transfer = std::make_shared<LocalShadowPageTransfer>(*lighting._submissions, remote.Plan.Device, target.Side, update.KeepPublication);
		const auto exported = transfer->Export(remote.Plan, remote.Bindings, remote.Work,
		    target.Texture.GetResource()->D3DResource.Get(), ownerSlices[page]);
		const auto order = ++lighting._shadowOrder;
		ShadowPublication* publication = nullptr;
		if (update.KeepPublication)
		{
			publication = &Publish(remote, update.Exact, order);
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
			WaitForOwner(remote, transfer->Import(primary, primaryBindings, lighting._work, LocalAtlas(), page));
			for (const auto& batch : batches)
				if (batch.Views.front().Tile.Page == page)
					for (const auto& view : batch.Views) AcceptExact(view, order);
		}
		remote.PageTransfers.push_back(std::move(transfer));
	}

	void ExportTiles(RemoteShadows& remote, const Batch& batch, const std::vector<UINT>& ownerSlices)
	{
		const auto& batchViews = batch.Views;
		const UINT side = batchViews.front().Tile.Side;
		const UINT count = UINT(batchViews.size());
		auto& target = *remote.Local->_slots.front().Depth;
		auto transfer = std::make_shared<LocalShadowTileTransfer>(*lighting._submissions, remote.Plan.Device, side, count, false);
		std::vector<GDX12LocalShadowTile> tiles;
		for (const auto& view : batchViews)
		{
			auto tile = view.Tile;
			tile.Page = ownerSlices[tile.Page];
			tiles.push_back(tile);
		}
		const auto exported = transfer->Export(remote.Plan, remote.Bindings, remote.Work, *remote.TileCopy,
		    target.Texture.GetResource()->D3DResource.Get(), std::move(tiles));
		ShadowPublication* publication = nullptr;
		const auto order = ++lighting._shadowOrder;
		for (UINT index = 0; index < count; ++index)
		{
			const auto& view = batchViews[index];
			const auto& light = source.Lights[view.Light];
			remote.FinalOrders[{light.EntityId, view.Face}] = order;
			if (lighting._localPageSync[view.Tile.Page] != ShadowCrossSync::Latest) continue;
			if (!publication)
			{
				publication = &Publish(remote, !batch.Latest, order);
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
			WaitForOwner(remote, transfer->Stage(primary, primaryBindings, lighting._work));
			if (publication) publication->LocalStaged = true;
			std::vector<LocalShadowTileCopy::Region> regions;
			for (UINT index = 0; index < count; ++index) regions.push_back(Region(index, batchViews[index].Tile));
			transfer->Publish(primary, primaryBindings, lighting._work, lighting.MainShadowTileCopy(), LocalAtlas(), std::move(regions));
			for (const auto& view : batchViews) AcceptExact(view, order);
		}
		remote.Transfers.push_back(std::move(transfer));
	}

	void ExportSun(const ShadowLane& lane, RemoteShadows& remote)
	{
		if (!remote.SunMask) return;
		const bool latest = lane.Latest;
		const bool publishSun = lighting._sunSync == 3;
		auto* heap = remote.Resources.SRV_UAV_Heap.get();
		if (!remote.SunTransfer || remote.SunTransfer.use_count() > 1)
			remote.SunTransfer = std::make_shared<SunShadowTileTransfer>(*lighting._submissions, remote.Plan.Device,
			    *lighting._resources.SRV_UAV_Heap, *heap, remote.SunTransfer.get());
		remote.SunTransfer->Prepare(lighting._resources.ShadowQuality.SunResolution, remote.SunMask);
		remote.SunTransfer->Export(remote.Plan, remote.Bindings, remote.Work, *remote.Sun->Output(0));
		if (publishSun)
		{
			auto& publication = Publish(remote, !latest, ++lighting._shadowOrder);
			publication.Sun = remote.SunTransfer;
			publication.SunResolution = lighting._resources.ShadowQuality.SunResolution;
			publication.SunMask = remote.SunMask;
			publication.SunRevision = lighting._resources.Lighting.SunShadowRevision;
			publication.SunSettings = sunSettings;
		}
		if (!latest)
		{
			const Id imported = remote.SunTransfer->Import(primary, primaryBindings, lighting._work, sun.Atlas);
			WaitForOwner(remote, imported);
			sun.Settings = sunSettings;
			sun.Revision = lighting._resources.Lighting.SunShadowRevision;
			sun.DirtyCascades &= ~remote.SunMask;
			if (publishSun) remote.Publications.front()->SunStaged = true;
			for (UINT tile = 0; tile < frame.SunShadowOrders.size(); ++tile)
				if (remote.SunMask & (1u << tile))
					frame.SunShadowOrders[tile] = publishSun ? remote.Publications.front()->Order : 0;
		}
	}

	void AppendOwner(const ShadowLane& lane, const std::vector<Batch>& batches,
	                 const std::unordered_set<ID3D12Resource*>& live, const std::vector<GDX12Texture*>& textures,
	                 std::map<Id, Id>& baselineExports)
	{
		const Id owner = lane.Owner;
		auto& owners = frame.ShadowOwners;
		const UINT descriptors = lane.Local ? RemoteShadows::DescriptorCount(live.size()) : 0;
		const auto existing = owners.find(lane);
		if (lane.Local && existing != owners.end() && existing->second->Produced &&
		    !lighting._submissions->Complete(existing->second->Produced))
		{
			lighting._localShadowOwnerBusy = true;
			FallbackToPrimary(batches);
			return;
		}
		if (existing != owners.end() && existing->second->Resources.SRV_UAV_Heap->GetNumDescriptors() < descriptors)
			owners.erase(existing); // This lane is idle. Rebind the mirrors in a larger heap.
		const auto previous = owners.find(lane);
		const auto* cached = previous == owners.end() ? nullptr : previous->second.get();
		std::vector<UINT> ownerSlices(lighting._localPageOwners.size());
		UINT ownerPages = 0;
		for (UINT page = 0; lane.Local && page < lighting._localPageOwners.size(); ++page)
			if (lighting._localPageOwners[page] == owner) ownerSlices[page] = ownerPages++;
		const auto pageUpdates = PlanPages(batches, cached);
		auto& entry = owners[lane];
		if (!entry) entry = std::make_unique<RemoteShadows>(lighting._submissions, owner, lighting._common, lighting._resources, descriptors);
		auto& remote = *entry;
		remote.SunMask = 0;
		if (!lane.Local)
			remote.SunMask = lighting._sunSync == 3 && !lane.Latest ? lighting._sunRemoteMask : (sun.Update & lighting._sunRemoteMask);
		remote.NearMask = sun.Update & ~lighting._sunRemoteMask;
		remote.Plan = {};
		remote.Plan.Device = owner;
		remote.Plan.FrameBoundary = false;
		remote.Bindings.clear();
		remote.Work.Commands.clear();
		remote.ExactExport = Invalid;
		remote.Lights.clear();
		remote.Count = 0;
		if (lane.Local) PrepareAtlases(remote, batches, ownerSlices, ownerPages);
		AppendOwnerScene(lane, remote, live, textures, baselineExports);
		// Exact exports share the DIRECT timeline. Submit them before optional Latest traffic.
		for (const bool latestResult : {false, true})
		{
			for (const auto& [page, update] : pageUpdates)
				if (update.Full && update.Latest == latestResult)
					ExportPage(remote, page, update, batches, ownerSlices);
			for (const auto& batch : batches)
				if (batch.Latest == latestResult && !pageUpdates.at(batch.Views.front().Tile.Page).Full)
					ExportTiles(remote, batch, ownerSlices);
		}
		ExportSun(lane, remote);
	}
};

void SceneLighting::AppendRemoteShadows(Graph& primary, std::vector<ID3D12Resource*>& primaryBindings,
                                            SceneGeometry& geometry)
{
	RemoteShadowPlan plan{*this, primary, primaryBindings, geometry};
	plan.CollectPublications();
	const auto faces = plan.ImportLocalFaces();
	const bool sunAvailable = plan.ImportSunCascades();
	const auto views = plan.SelectBatches(faces, sunAvailable);
	if (views.empty()) return;

	auto live = geometry.LiveProbeInputs();
	const auto textures = plan.CollectMaskedTextures(live);
	live.insert(_resources.FrameConstants.at(_slot)->CameraCB->GetResource().D3DResource.Get());
	std::map<Id, Id> baselineExports;
	for (const auto& [lane, batches] : views)
		plan.AppendOwner(lane, batches, live, textures, baselineExports);
}

void SceneLighting::AddShadowExchanges(const Schedule& primary, std::vector<Executor::PacketExchange>& exchanges)
{
	for (auto& [lane, entry] : _frames.at(_slot).ShadowOwners)
	{
		auto& remote = *entry;
		if (remote.Import != Invalid) remote.Import = primary.PacketFor(remote.Import);
		if (remote.Export == Invalid) continue;
		auto schedule = Compile(remote.Plan);
		const Id input = schedule.PacketFor(remote.Input);
		const Id seed = schedule.OptionalPacketFor(remote.SeedVertices);
		const Id exactExport = schedule.OptionalPacketFor(remote.ExactExport);
		// Only optional Latest exports can follow the final local Exact export in this plan.
		const Id signalAfter = exactExport != Invalid && remote.ExactExport + 1 < remote.Plan.Operations.size() ?
		    exactExport : Invalid;
		remote.GPU.Begin(0);
		remote.GPU.Reserve(schedule);
		exchanges.push_back({primary.PacketFor(remote.Export), remote.Import,
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
} // namespace BistroRender

#pragma once
#include "NativeWork.h"
#include "GeometryTarget.h"
#include "ShadowMemory.h"
#include "Engine.Core/Jobs/JobSystem.h"
#include "Engine.RendererDX12/GDX12DeviceResources.h"
#include <optional>
#include <functional>
#include <unordered_map>

class RenderPipelineCommonData;

namespace BistroRender
{
class QueueSubmission;

class SceneLocalShadows
{
	struct Atlas;
  public:
	SceneLocalShadows(GDX12DeviceResources&, RenderPipelineCommonData&, bool staticOnly = false, bool remote = false);
	~SceneLocalShadows();
	void Freeze(UINT slot, const std::vector<GDX12RenderPrimitive>& grass, UINT width, UINT height);
	void Complete(UINT slot);
	void Submitted(UINT slot);
	uint64_t SceneRevision() const { return _sceneRevision; }
	const std::vector<BoundingBox>& ChangedCasterBounds() const { return _changedCasterBounds; }
	bool CastersRebuilt() const { return _castersRebuilt; }
	GDX12UploadBuffer<GDX12LocalLightConstants>& LightSource(UINT slot) { return *_slots.at(slot).LightUpload; }
	void Append(
	    Graph&, std::vector<ID3D12Resource*>&, UINT slot, const std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 4>& scene,
	    GDX12GeometryBuffer::RecordingBinding geometry, GDX12UploadBuffer<GDX12LocalLightConstants>& lights,
	    const std::function<void(const GeometryTarget&, Graph&, std::vector<ID3D12Resource*>&)>& appendGrass = {},
	    const std::array<Id, 5>* inputs = nullptr, const std::vector<Access>& materialTextures = {},
	    Atlas* fallbackDepth = nullptr);
	bool Record(UINT slot, Id id, ID3D12GraphicsCommandList* cmd) const { return _slots.at(slot).Work.Record(id, cmd); }
	GDX12Texture* Output(UINT slot);
	bool HasAtlas(UINT slot) const { return bool(_slots.at(slot).Depth); }
	uint64_t ContentVersion(UINT slot, UINT light, UINT face) const
	{ return _slots.at(slot).Cache.at(light).ContentVersions.at(face); }
	std::string Distribution(UINT slot) const;
	UINT64 ShadowTexels(UINT slot) const;

  private:
	friend class SceneLighting;
	using GrassAppender = std::function<void(const GeometryTarget&, Graph&, std::vector<ID3D12Resource*>&)>;
	void AppendGrassViews(Graph& graph, std::vector<ID3D12Resource*>& bindings, UINT slot,
	                      Id atlas, Id fallback, const Atlas* fallbackDepth, const GrassAppender& appendGrass);
	struct Input
	{
		GDX12IndirectDrawArgs Command{};
		GDX12InstanceData Instance{};
		GDX12MaterialConstants Material;
		XMFLOAT4X4 World{};
		uint64_t DeformationRevision = 0;
		bool Dynamic = false;
	};
	struct Caster
	{
		BoundingBox Bounds;
		GDX12IndirectDrawArgs Command;
		bool CastsShadow, Masked;
		UINT InputIndex;
		bool Dynamic;
	};
	struct Node
	{
		BoundingBox Bounds;
		UINT First = 0, Count = 0, Left = 0, Right = 0, Parent = UINT_MAX;
	};
	struct FaceCommands
	{
		std::vector<GDX12IndirectDrawArgs> Opaque, Masked;
	};
	struct LightCache
	{
		GDX12LocalLightConstants Light;
		std::array<FaceCommands, 6> Faces;
		std::optional<std::array<GDX12LocalShadowFrustum, 6>> Frusta;
		std::array<GDX12LocalShadowTile, 6> Tiles;
		std::array<uint64_t, 6> ContentVersions{}, PublicationOrders{};
		UINT Resolution = 0, Needed = 0, ListsValid = 0, MapsValid = 0;
		float ScreenPixels = 0;
		UINT LocalFallback = 0, ResidentFaces = 0, ReadyFaces = 0;
		uint64_t Generation = 0;
		bool Latest = false;
		bool HasLight = false;
		bool SelectionDirty = true;
	};
	struct View
	{
		UINT Light, Face, First, OpaqueCount, MaskedCount;
		GDX12LocalShadowTile Tile;
		bool DrawIntoFinal = false;
	};
	struct Slot;
	struct CullJob final : ParallelJob
	{
		SceneLocalShadows* Owner = nullptr;
		Slot* Frame = nullptr;
		void Execute(size_t index) override;
	};
	struct Atlas
	{
		ShadowMemory::Charge Memory;
		GDX12Texture Texture;
		std::vector<std::unique_ptr<GDX12Descriptor>> Dsvs;
		UINT Side = 0, Pages = 0;
	};
	struct Slot
	{
		std::vector<GDX12LocalLightConstants> Lights;
		std::vector<LightCache> Cache;
		std::vector<View> Views;
		std::vector<View> RemoteViews;
		std::vector<GDX12IndirectDrawArgs> Packed;
		std::optional<GDX12LocalShadowFrustum> Camera;
		XMFLOAT4X4 CameraMatrix{};
		bool SelectionChanged = true;
		UINT Width = 0, Height = 0, Ceiling = 0;
		std::array<XMFLOAT4, 6> CameraPlanes;
		std::array<XMFLOAT3, 8> CameraCorners;
		CullJob Cull;
		JobHandle Ready;
		std::unique_ptr<GDX12UploadBuffer<GDX12LocalLightConstants>> LightUpload;
		std::unique_ptr<GDX12UploadBuffer<GDX12IndirectDrawArgs>> CommandUpload, Commands;
		std::unique_ptr<Atlas> Depth;
		std::vector<UINT64> OccupiedTexels;
		UINT Placed = 0, Downgraded = 0, Skipped = 0;
		uint64_t SceneRevision = 0, GrassReceiverRevision = 0, GrassCasterRevision = 0;
		NativeWork Work;
	};
	static void PackView(Slot& target, const FaceCommands& commands, UINT light, UINT face, GDX12LocalShadowTile tile);
	static void DrawShadowViews(ID3D12GraphicsCommandList* cmd, const Slot& frame, ID3D12CommandSignature* signature,
	                            ID3D12Resource* target, ID3D12PipelineState* opaquePipeline,
	                            ID3D12PipelineState* maskedPipeline, bool clearDepth, const Atlas* fallbackDepth);
	void UpdateCaster(UINT position, const Input& current);
	void UpdateCamera(Slot&);
	void UpdateLights(Slot&);
	UINT BuildNode(UINT first, UINT count, UINT parent = UINT_MAX);
	void Refit(UINT caster);
	void InvalidateBounds(const BoundingBox&, bool listsChanged);
	void Invalidate(const Caster&, bool listsChanged);
	bool InvalidateGrass(const GDX12RenderPrimitive&);
	void UpdateGrass(const std::vector<GDX12RenderPrimitive>&);
	void UpdateCasters(UINT slot);
	void Gather(UINT node, const GDX12LocalLightConstants&, const std::array<GDX12LocalShadowFrustum, 6>&,
	            UINT faceMask, std::array<FaceCommands, 6>&) const;
	struct ScreenCoverage
	{
		bool Full = false;
		std::vector<std::array<float, 4>> Rectangles;
		void Add(const BoundingBox& receiver, const BoundingBox& influence, const XMFLOAT4X4& camera);
		double Area() const;
	};
	UINT NeededFaces(UINT node, const GDX12LocalLightConstants&, const std::array<GDX12LocalShadowFrustum, 6>&,
	                 UINT faceMask, const Slot&, const BoundingBox& influence, ScreenCoverage& coverage) const;
	void RebuildCasters(UINT slot);
	Input ReadInput(UINT slot, UINT command) const;
	std::unique_ptr<Atlas> CreateAtlas(UINT pageSide, UINT pages);
	void PrepareAtlas(Slot&, UINT pageSide, UINT pages);
	void PrepareAtlases(UINT pageSide, UINT pages);
	void MarkDepthWritten(UINT slot, UINT light, UINT face);
	void Allocate(UINT slot, SceneLocalShadows& staticPass, UINT effectiveCeiling);
	void UpdateLayout(const Slot&, UINT effectiveCeiling);
	void Pack(Slot&);
	void GatherMissing(Slot&, UINT lightIndex, UINT faces);
	void ResetLayout();
	GDX12Texture _fallback;
	struct Placement
	{
		UINT Resolution;
		std::array<GDX12LocalShadowTile, 6> Tiles;
		uint64_t Generation;
	};
	struct PlacementRequest
	{
		UINT Index, Faces, Desired;
		float Pixels;
		std::pair<UINT, uint64_t> Key;
		bool operator==(const PlacementRequest& other) const
		{
			return Index == other.Index && Faces == other.Faces && Desired == other.Desired &&
			    Pixels == other.Pixels && Key == other.Key;
		}
	};
	struct AtlasLayout
	{
		UINT Ceiling = 0, Side = 0, Pages = 0;
		UINT Placed = 0, Downgraded = 0, Skipped = 0;
		std::vector<PlacementRequest> Requests;
		std::vector<Placement> Placements;
		std::vector<UINT64> OccupiedTexels;
	};
	AtlasLayout _layout;
	std::vector<PlacementRequest> _placementRequests;
	std::map<std::pair<UINT, uint64_t>, Placement> _previousPlacement;
	uint64_t _generation = 0;
	GDX12DeviceResources& _resources;
	RenderPipelineCommonData& _common;
	std::unique_ptr<GDX12DescriptorHeap> _dsvHeap;
	std::vector<Slot> _slots;
	std::vector<Caster> _casters;
	std::vector<BoundingBox> _changedCasterBounds;
	std::vector<Node> _nodes;
	UINT _staticRoot = UINT_MAX, _dynamicRoot = UINT_MAX;
	std::vector<Input> _inputs;
	std::vector<UINT> _changedCommands;
	std::vector<UINT> _commandGeneration, _materialGeneration;
	UINT _changeGeneration = 0;
	std::vector<GDX12RenderPrimitive> _grass;
	std::vector<GDX12LocalShadowKey> _latestPlacements;
	std::unordered_map<UINT, std::vector<UINT>> _instances, _transforms, _materials;
	std::vector<UINT> _positions, _leaves;
	bool _treeReady = false, _castersRebuilt = false;
	bool _staticOnly = false;
	uint64_t _depthVersion = 0;
	uint64_t _sceneRevision = 0, _geometryRevision = 0, _meshRevision = 0, _textureRevision = 0;
	uint64_t _grassReceiverRevision = 0;
	uint64_t _grassCasterRevision = 0;
	std::unique_ptr<GDX12RootSignature> _root;
	ComPtr<ID3D12PipelineState> _opaqueDraw, _maskedDraw;
	ComPtr<ID3D12CommandSignature> _signature;
};
} // namespace BistroRender

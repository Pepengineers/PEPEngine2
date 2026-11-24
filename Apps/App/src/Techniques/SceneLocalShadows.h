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
	    const std::array<Id, 5>* inputs = nullptr, const std::vector<Access>& materialTextures = {});
	bool Record(UINT slot, Id id, ID3D12GraphicsCommandList* cmd) const { return _slots.at(slot).Work.Record(id, cmd); }
	GDX12Texture* Output(UINT slot);
	std::vector<GDX12Texture*> Outputs(UINT slot);
	const std::vector<uint64_t>& ContentVersions(UINT slot, UINT resolution) const
	{ return _slots.at(slot).Atlases.at(resolution).ContentVersions; }
	std::string Distribution(UINT slot);
	UINT64 ShadowTexels(UINT slot) const;

  private:
	friend class SceneLighting;
	using GrassAppender = std::function<void(const GeometryTarget&, Graph&, std::vector<ID3D12Resource*>&)>;
	void AppendGrassViews(Graph& graph, std::vector<ID3D12Resource*>& bindings, UINT slot, const std::map<std::pair<UINT, bool>, Id>& atlases,
	                      const GrassAppender& appendGrass);
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
		UINT FirstSlice = UINT_MAX, Resolution = 0, Needed = 0, ListsValid = 0, MapsValid = 0;
		float ScreenPixels = 0;
		UINT LocalFallback = 0, ResidentFaces = 0, ReadyFaces = 0;
		UINT StagedFaces = 0, StagedValid = 0, StageThisFrame = 0;
		uint64_t Generation = 0;
		Id Owner = 0;
		bool Latest = false;
		bool HasLight = false;
		bool SelectionDirty = true;
	};
	struct View
	{
		UINT Light, Face, Slice, First, OpaqueCount, MaskedCount, Resolution;
		bool Dirty = true;
		bool Staged = false;
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
		std::vector<uint64_t> ContentVersions;
		UINT Slices = 0;
	};
	struct ShadowLayout
	{
		std::vector<UINT> Sizes, Starts;
		std::map<UINT, UINT> Capacities;
	};
	struct BudgetRange
	{
		UINT64 Minimum = 0, Maximum = UINT64_MAX;
		bool Fits(UINT64 available, UINT64 required);
		bool Contains(UINT64 value) const { return value >= Minimum && value <= Maximum; }
	};
	struct AllocationPlan
	{
		UINT Ceiling = 0;
		Id TransparencyOwner = 0;
		uint64_t PlacementRevision = 0;
		std::vector<std::pair<UINT, float>> Receivers;
		std::map<Id, BudgetRange> Budgets;
		BudgetRange RetiredSlot;
		std::vector<UINT> Tiers;
		bool Limited = false, Settled = false;
		std::string Limit;
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
		bool Indexed = false;
		std::array<XMFLOAT4, 6> CameraPlanes;
		std::array<XMFLOAT3, 8> CameraCorners;
		CullJob Cull;
		JobHandle Ready;
		std::unique_ptr<GDX12UploadBuffer<GDX12LocalLightConstants>> LightUpload;
		std::unique_ptr<GDX12UploadBuffer<GDX12IndirectDrawArgs>> CommandUpload, Commands;
		std::map<UINT, Atlas> Atlases, StagedAtlases;
		std::optional<ShadowLayout> StagedLayout;
		std::optional<AllocationPlan> Plan;
		auto& AtlasFor(UINT resolution, bool staged) { return (staged ? StagedAtlases : Atlases).at(resolution); }
		const auto& AtlasFor(UINT resolution, bool staged) const { return (staged ? StagedAtlases : Atlases).at(resolution); }
		std::string Limit;
		uint64_t SceneRevision = 0, GrassReceiverRevision = 0, GrassCasterRevision = 0;
		uint64_t LayoutRevision = 0, MigrationTexels = 0;
		NativeWork Work;
	};
	static void DrawShadowViews(ID3D12GraphicsCommandList* cmd, const Slot& frame, ID3D12CommandSignature* signature,
	                            ID3D12Resource* target, ID3D12PipelineState* opaquePipeline,
	                            ID3D12PipelineState* maskedPipeline, bool clearDepth);
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
	void PrepareAtlas(Slot&, UINT resolution, UINT slices, bool staged = false);
	void MarkDepthWritten(UINT slot, UINT resolution, UINT slice, bool staged = false);
	void Allocate(UINT slot, SceneLocalShadows& staticPass, const QueueSubmission& submissions, Id transparencyOwner);
	void ApplyLayout(Slot&, const std::vector<UINT>& sizes, const std::vector<UINT>& slices);
	void Pack(Slot&);
	void GatherMissing(Slot&, UINT lightIndex, UINT faces);
	void ResetLayout();
	GDX12Texture _fallback;
	struct Placement
	{
		UINT Resolution;
		uint64_t Generation;
	};
	std::map<std::pair<UINT, uint64_t>, Placement> _previousResolution;
	uint64_t _generation = 0, _placementRevision = 0;
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
	std::vector<std::pair<GDX12LocalShadowKey, Id>> _placements;
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

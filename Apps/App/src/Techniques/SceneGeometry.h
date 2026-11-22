#pragma once
#include "Graph/RenderGraphExecutor.h"
#include "Engine.RendererDX12/GDX12DeviceResources.h"
#include "Engine.RendererDX12/GDX12Skinning.h"
#include "Techniques/Grass/SceneGrass.h"
#include "Linden/LindenDraw.h"
#include "Linden/LindenSimulation.h"
#include "Linden/LindenMesh.h"
#include "NativeWork.h"
#include <array>
#include <map>
#include <unordered_set>

namespace BistroRender
{
class SceneLighting;
class SceneReflections;
class ProbeInputMirror;
struct CanvasRead;
// Owner-only Freeze/Resize. Record uses immutable slot data and native commands;
// no shared resource tracker, heap allocator, ECS or queue access on workers.
class SceneGeometry
{
  public:
	explicit SceneGeometry(RenderPipelineContext&);
	static bool DepthNormalsEnabled();
	void PrepareVegetation(RenderPipelineContext& c, float dt, Executor&);
	SceneGrass& Grass() { return *_grass; }
	std::vector<GDX12RenderPrimitive> VegetationPrimitives() const;
	void AppendVegetationView(Graph&, std::vector<ID3D12Resource*>&, UINT slot, const GeometryTarget&);
	std::unordered_set<ID3D12Resource*> LiveProbeInputs() const;
	class RemoteProbeVegetation
	{
	  public:
		explicit RemoteProbeVegetation(GDX12DeviceResources& owner) : Grass(owner) {}
		void Forget(ID3D12Resource*);

	  private:
		friend class SceneGeometry;
		SceneGrass::ProbeCapture Grass;
		std::unique_ptr<LindenDraw> Linden;
		std::map<std::pair<ID3D12Resource*, UINT>, std::unique_ptr<GDX12Descriptor>> Views;
	};
	void AppendVegetationView(Graph& primary, std::vector<ID3D12Resource*>& primaryBindings, ProbeInputMirror&,
	                          Graph& remote, std::vector<ID3D12Resource*>& remoteBindings, NativeWork&,
	                          RemoteProbeVegetation&, UINT slot, const GeometryTarget&,
	                          const GDX12Descriptor& remoteMaterials, Id material, Id main,
	                          D3D12_GPU_VIRTUAL_ADDRESS mainAddress, D3D12_GPU_DESCRIPTOR_HANDLE localLights);
	GDX12ParticleInteractionField InteractionField() const { return _grass->InteractionField(); }

	void Resize(UINT width, UINT height); // caller has drained all slots
	void Freeze(UINT slot, UINT camera, std::vector<GDX12SkinningWork> skin,
	            const std::shared_ptr<QueueSubmission>&);
	void SubmitSkinning(UINT slot, const Schedule&, std::vector<PacketWait>&);
	void SkinningSubmitted(UINT slot, uint64_t order, CompletionToken);
	void RetireSkinning(UINT slot, const QueueSubmission&);
	void DrainSkinning();
	void AppendSkinningOwners(UINT slot, std::vector<std::pair<std::string, Id>>& owners) const
	{
		const auto& frame = _slots.at(slot);
		if (!frame.Skin.empty()) owners.emplace_back("Skinning", 0);
		for (const auto& [id, remote] : frame.RemoteSkin)
			if (!remote.Ranges.empty()) owners.emplace_back("Skinning", id);
	}
	using AppendTechnique = std::function<void(Graph&, std::vector<ID3D12Resource*>&)>;
	using AppendDepthNormals = std::function<void(Graph&, std::vector<ID3D12Resource*>&, Id ready)>;
	void AppendWorldCanvases(Graph&, UINT slot, const std::vector<CanvasRead>&);
	std::shared_ptr<const Schedule> Plan(UINT slot, const AppendTechnique& append = {}, const AppendTechnique& prelude = {},
	              bool singleDirect = true, const AppendTechnique& afterInteraction = {},
	              const AppendTechnique& afterDepth = {}, const AppendDepthNormals& afterDepthNormals = {});
	const std::vector<ID3D12Resource*>& PlannedResources(UINT slot) const { return _slots.at(slot).PlannedBindings; }
	// Frame-slot inputs for future graph consumers; access declarations and the
	// frame completion fence still govern their lifetime and readiness.
	GDX12Texture* Depth(UINT slot) const { return _slots.at(slot).DepthView.get(); }
	GDX12Texture* PrepassNormals(UINT slot) const
	{
		const auto& frame = _slots.at(slot);
		return frame.DepthNormals ? frame.EarlyNormals.get() : nullptr;
	}
	D3D12_CPU_DESCRIPTOR_HANDLE ReadOnlyDepthDSV(UINT slot) const { return _slots.at(slot).ReadDepth.CPUHandle; }
	D3D12_CPU_DESCRIPTOR_HANDLE DepthDSV(UINT slot) const { return _slots.at(slot).Dsv.CPUHandle; }
	GDX12Texture* GBuffer(UINT slot, UINT attachment) const { return _slots.at(slot).Color.at(attachment).get(); }
	std::vector<ID3D12Resource*> Resources(UINT slot) const;
	void Record(UINT slot, Id operation, UINT bucket, ID3D12GraphicsCommandList*) const;

  private:
	friend class SceneTransparency;
	friend class SceneLighting;
	friend class SceneReflections;
	friend class SceneProbePreview;
	struct Buffer
	{
		GDX12ResourceHandle Source, Data, Origin;
		uint64_t Revision = 0;
		bool Upload = false;
		std::vector<std::pair<UINT, UINT>> Ranges;
		UINT64 Bytes = 0;
		GDX12Descriptor Srv, Uav;
	};
	struct RemoteSkinning
	{
		RemoteSkinning(const std::shared_ptr<QueueSubmission>& submit, Id owner) : GPU(submit, owner, 1, 1) {}
		struct Range
		{
			GDX12SkinningWork Work;
			UINT FirstVertex;
		};
		std::vector<Range> Ranges;
		UINT VertexCount = 0;
		std::shared_ptr<GDX12ResourceHandle> Output;
		QueueSubmission::TransferBuffer Transfer;
		UINT64 TransferBytes = 0;
		Executor GPU;
	};
	struct Slot
	{
		Graph CompiledGraph;
		std::vector<Id> CompiledOrder;
		bool SingleDirect = false;
		std::shared_ptr<const Schedule> CompiledSchedule;
		NativeWork LindenWork;
		LindenDisplayedPose LindenPose{};
		std::shared_ptr<const LindenMesh> Linden;
		std::vector<ID3D12Resource*> PlannedBindings;
		std::vector<std::pair<Id, Id>> ShadowInputCopies;
		std::array<Buffer, 8> Buffers;      // instances/templates/materials/transforms/opaque/count/transparent/count
		std::array<Buffer, 2> FrameBuffers; // immutable main/camera constants
		std::array<Id, 2> FrameIds{};
		std::array<UINT, 2> FrameStrides{};
		std::array<std::unique_ptr<GDX12Texture>, 5> Color;
		std::unique_ptr<GDX12Texture> DepthView, EarlyNormals;
		Id DepthNormalsReady = Invalid;
		bool DepthNormals = false;
		GDX12ResourceHandle Depth;
		GDX12Descriptor Dsv, ReadDepth;
		GDX12GeometryBuffer::RecordingBinding Geometry{};
		D3D12_GPU_VIRTUAL_ADDRESS Camera = 0, Main = 0;
		UINT Draws = 0;
		std::vector<GDX12IndirectDrawArgs> CanvasDraws;
		Id CanvasOperation = Invalid;
		std::vector<GDX12SkinningWork> Skin;
		std::vector<ID3D12Resource*> SkinBindings;
		std::vector<Resource> SkinDeclarations;
		std::vector<std::array<Id, 5>> SkinIds;
		std::map<Id, RemoteSkinning> RemoteSkin;
		NativeWork SkinImports;
		Id SkinImport = Invalid;
		uint64_t SkinOrder = 0;
		CompletionToken SkinConsumer;
		std::unique_ptr<GDX12InstanceBatcher> Batcher;
		GDX12ResourceHandle BatchCounter;
		UINT Batches = 0, InstanceCount = 0;
		std::array<Id, 5> BatchIds{};
	};
	struct BasePlan
	{
		Graph Template;
		std::vector<std::array<Id, 5>> SkinIds;
		UINT UploadMask = 0;
		bool Batched = false, DepthNormals = false;
		std::array<Id, 5> BatchIds{};
		std::array<Id, 2> FrameIds{};
	} _basePlan;
	static Graph BuildBasePlan(Slot&);
	void AppendSkinImports(Graph&, Slot&);
	std::vector<std::pair<UINT, UINT>> ChangedVertexRanges(uint64_t since) const;
	void RecordUploads(const Slot&, UINT bucket, ID3D12GraphicsCommandList*) const;
	enum class GeometryPass { Depth, GBuffer, WorldCanvas };
	void RecordGeometry(const Slot&, GeometryPass, ID3D12GraphicsCommandList*) const;
	void BufferFor(Buffer&, UINT64 bytes, UINT stride, bool upload = false);
	GDX12DeviceResources& _resources;
	std::vector<Slot> _slots;
	uint64_t _vertexDeformationRevision = 0;
	GDX12ResourceHandle _vertexChangeSource;
	// Each boundary starts a span with the same last write revision.
	std::map<UINT, uint64_t> _vertexWriteRevisions{{0, 0}};
	std::unordered_set<Id> _reportedSkinOwners;
	std::unique_ptr<GDX12RootSignature> _cullRoot, _clearRoot, _drawRoot;
	std::unique_ptr<SceneGrass> _grass;
	LindenSimulation _lindenSimulation;
	LindenDraw _lindenDraw;
	std::shared_ptr<const LindenMesh> _linden;
	RenderPipelineCommonData& _common;
	GDX12ResourceHandle _emptyVertices, _emptyIndices;
	ComPtr<ID3D12PipelineState> _cull, _clear, _depth, _depthNormals, _gbuffer, _worldCanvas;
	ComPtr<ID3D12CommandSignature> _signature;

	UINT _width = 0, _height = 0;
};
} // namespace BistroRender

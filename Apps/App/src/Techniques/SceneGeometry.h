#pragma once
#include "Graph/MetaGraphExecutor.h"
#include "Engine.RendererDX12/GDX12DeviceResources.h"
#include "Engine.RendererDX12/GDX12Skinning.h"
#include "SceneGrass.h"
#include <array>

namespace Meta
{
class SceneLighting;
class SceneReflections;
// Owner-only Freeze/Resize. Record uses immutable slot data and native commands;
// no shared resource tracker, heap allocator, ECS or queue access on workers.
class SceneGeometry
{
  public:
	explicit SceneGeometry(RenderPipelineContext &);
	void PrepareGrass(RenderPipelineContext &c, float dt) { _grass->Prepare(c, dt); }
	std::vector<GDX12RenderPrimitive> GrassPrimitives() const { return _grass->Primitives(); }
	void AppendGrassView(Graph &, std::vector<ID3D12Resource *> &, UINT slot, const GeometryTarget &);
	GDX12ParticleInteractionField InteractionField() const { return _grass->InteractionField(); }

	void Resize(UINT width, UINT height); // caller has drained all slots
	void Freeze(UINT slot, UINT camera, std::vector<GDX12SkinningWork> skin);
	using AppendTechnique = std::function<void(Graph &, std::vector<ID3D12Resource *> &)>;
	Schedule Plan(UINT slot, const AppendTechnique &append = {}, const AppendTechnique &prelude = {},
	              bool singleDirect = true);
	const std::vector<ID3D12Resource *> &PlannedResources(UINT slot) const { return _slots.at(slot).PlannedBindings; }
	// Frame-slot inputs for future graph consumers; access declarations and the
	// frame completion fence still govern their lifetime and readiness.
	GDX12Texture *Depth(UINT slot) const { return _slots.at(slot).DepthView.get(); }
	GDX12Texture *GBuffer(UINT slot, UINT attachment) const { return _slots.at(slot).Color.at(attachment).get(); }
	std::vector<ID3D12Resource *> Resources(UINT slot) const;
	void Record(UINT slot, Id operation, UINT bucket, ID3D12GraphicsCommandList *) const;

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
	struct Slot
	{
		std::vector<ID3D12Resource *> PlannedBindings;
		std::array<Buffer, 8> Buffers;      // instances/templates/materials/transforms/opaque/count/transparent/count
		std::array<Buffer, 2> FrameBuffers; // immutable main/camera constants
		std::array<Id, 2> FrameIds{};
		std::array<UINT, 2> FrameStrides{};
		std::array<std::unique_ptr<GDX12Texture>, 5> Color;
		std::unique_ptr<GDX12Texture> DepthView;
		GDX12ResourceHandle Depth;
		GDX12Descriptor Dsv, ReadDepth;
		GDX12GeometryBuffer::RecordingBinding Geometry{};
		D3D12_GPU_VIRTUAL_ADDRESS Camera = 0, Main = 0;
		UINT Draws = 0;
		std::vector<GDX12SkinningWork> Skin;
		std::vector<ID3D12Resource *> SkinBindings;
		std::vector<Resource> SkinDeclarations;
		std::vector<std::array<Id, 5>> SkinIds;
		std::unique_ptr<GDX12InstanceBatcher> Batcher;
		GDX12ResourceHandle BatchCounter;
		UINT Batches = 0, InstanceCount = 0;
		std::array<Id, 5> BatchIds{};
	};
	void RecordUploads(const Slot &, UINT bucket, ID3D12GraphicsCommandList *) const;
	void RecordGeometry(const Slot &, bool depth, ID3D12GraphicsCommandList *) const;
	void BufferFor(Buffer &, UINT64 bytes, UINT stride, bool upload = false);
	GDX12DeviceResources &_resources;
	std::vector<Slot> _slots;
	std::unique_ptr<GDX12RootSignature> _cullRoot, _clearRoot, _drawRoot;
	std::unique_ptr<SceneGrass> _grass;
	RenderPipelineCommonData &_common;
	GDX12ResourceHandle _emptyVertices, _emptyIndices;
	ComPtr<ID3D12PipelineState> _cull, _clear, _depth, _gbuffer;
	ComPtr<ID3D12CommandSignature> _signature;

	UINT _width = 0, _height = 0;
};
} // namespace Meta

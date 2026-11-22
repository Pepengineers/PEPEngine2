#pragma once
#include "NativeWork.h"
#include "GeometryTarget.h"
#include "Engine.RendererDX12/GDX12DeviceResources.h"
#include <functional>

class RenderPipelineCommonData;

namespace BistroRender
{
class SceneSunShadows
{
  public:
	SceneSunShadows(GDX12DeviceResources&, RenderPipelineCommonData&);
	void Freeze(UINT slot, bool castersRebuilt, const std::vector<BoundingBox>& changedCasters,
	            const std::vector<GDX12RenderPrimitive>& grass = {});
	void Append(
	    Graph&, std::vector<ID3D12Resource*>&, UINT slot, const std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 4>& scene,
	    UINT draws, GDX12GeometryBuffer::RecordingBinding geometry,
	    const std::function<void(const GeometryTarget&, Graph&, std::vector<ID3D12Resource*>&)>& appendGrass = {},
	    UINT mask = UINT_MAX, const std::array<Id, 6>* inputs = nullptr,
	    const std::vector<Access>& materialTextures = {});
	bool Record(UINT slot, Id id, ID3D12GraphicsCommandList* cmd) const { return _slots.at(slot).Work.Record(id, cmd); }
	GDX12Texture* Output(UINT slot) { return &_slots.at(slot).Atlas; }

  private:
	friend class SceneLighting;
	using GrassAppender = std::function<void(const GeometryTarget&, Graph&, std::vector<ID3D12Resource*>&)>;
	void AppendGrassViews(Graph& graph, std::vector<ID3D12Resource*>& bindings, UINT slot, Id atlas,
	                      UINT update, const GrassAppender& appendGrass);
	struct VegetationCaster
	{
		uint64_t Revision;
		BoundingBox Bounds;
	};
	struct Slot
	{
		GDX12Texture Atlas;
		std::array<std::unique_ptr<GDX12UploadBuffer<GDX12IndirectDrawArgs>>, 4> Commands;
		std::unique_ptr<GDX12UploadBuffer<UINT>> Counters;
		NativeWork Work;
		BoundingBox Bounds{};
		bool HasBounds = false, BoundsReady = false, Rendered = false;
		UINT Resolution = UINT_MAX, Revision = 0, Update = 0, DirtyCascades = 0;
		Id Owner = 0;
		UINT Near = UINT_MAX;
		std::vector<VegetationCaster> Vegetation;
		GDX12ShadowConstants Settings{};
	};
	void PrepareAtlas(Slot&, UINT resolution);
	void PrepareIndirectBuffers(Slot&, UINT draws);
	void RebuildBounds(Slot&, UINT slot);
	GDX12DeviceResources& _resources;
	RenderPipelineCommonData& _common;
	std::vector<Slot> _slots;
	std::unique_ptr<GDX12RootSignature> _graphicsRoot, _computeRoot;
	ComPtr<ID3D12PipelineState> _opaqueDraw, _maskedDraw, _cull, _clear;
	ComPtr<ID3D12CommandSignature> _signature;
};
} // namespace BistroRender

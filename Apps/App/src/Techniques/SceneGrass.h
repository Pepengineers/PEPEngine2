#pragma once
#include "Rendering/Grass/GDX12GrassRenderer.h"
#include "GeometryTarget.h"
#include "App.Base/Rendering/IRenderPipeline.h"
#include "Rendering/Grass/BistroGrassGeometry.h"
#include <functional>

namespace BistroRender
{
class SceneGrass
{
  public:
	explicit SceneGrass(RenderPipelineContext&);
	void Prepare(RenderPipelineContext&, float dt);
	GDX12ParticleInteractionField InteractionField() const { return _gpu->InteractionField(); }
	std::vector<GDX12RenderPrimitive> Primitives() const;
	void Append(Graph&, UINT slot, std::vector<ID3D12Resource*>& bindings, GDX12Descriptor& materials,
	            const std::vector<GeometryTarget>& targets, bool viewsOnly = false);
	bool Record(UINT slot, Id, ID3D12GraphicsCommandList*) const;

  private:
	struct RecordedFrame
	{
		struct MainGeometry
		{
			std::unique_ptr<GDX12UploadBuffer<UINT>> Indices, Arguments;
		};
		std::vector<MainGeometry> MainPatches;
		struct BuiltPatch
		{
			std::unique_ptr<GDX12UploadBuffer<GDX12GrassRenderer::Vertex>> Vertices;
			BoundingBox Bounds;
		};
		std::vector<BuiltPatch> Built;
		std::unique_ptr<GDX12UploadBuffer<float>> HiZ;
		std::vector<std::pair<Id, std::function<void(ID3D12GraphicsCommandList*)>>> Commands;
	};
	GDX12DeviceResources& _resources;
	BistroGrassGeometry _scene;
	std::unique_ptr<GDX12GrassRenderer> _gpu;
	std::vector<RecordedFrame> _frames;
	std::unique_ptr<GDX12RootSignature> _geometryRoot, _hiZRoot;
	ComPtr<ID3D12PipelineState> _buildGeometry, _cullGeometry, _clearGeometry, _buildHiZ;
};
} // namespace BistroRender

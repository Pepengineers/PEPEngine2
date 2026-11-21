#pragma once
#include "GDX12GrassRenderer.h"
#include "Techniques/GeometryTarget.h"
#include "App.Base/Rendering/IRenderPipeline.h"
#include "BistroGrassGeometry.h"
#include "Graph/RenderGraphExecutor.h"
#include "Techniques/NativeWork.h"
#include <functional>

namespace BistroRender
{
class LindenSimulation;
struct FragmentPose;
struct LindenMesh;
struct LindenWindSample
{
	GDX12UploadBuffer<XMFLOAT4>* Grid = nullptr;
	GDX12UploadBuffer<FragmentPose>* Response = nullptr;
	XMFLOAT4 Area{};
	float Time = 0;
	UINT Resolution = 0;
	uint64_t Revision = 0;
};
class SceneGrass
{
  public:
	explicit SceneGrass(RenderPipelineContext&);
	~SceneGrass();
	void Prepare(RenderPipelineContext&, float dt, Executor&, const std::shared_ptr<const LindenMesh>&, const LindenSimulation&);
	Id WindExecutionDevice() const;
	void AddWindWaits(const Schedule&, std::vector<PacketWait>&);
	void WindSubmitted(uint64_t frame, const std::vector<CompletionToken>&);
	const LindenWindSample& LindenWind() const { return _lindenWind; }
	GDX12ParticleInteractionField InteractionField() const { return _gpu->InteractionField(); }
	std::vector<GDX12RenderPrimitive> Primitives() const;
	void Append(Graph&, UINT slot, std::vector<ID3D12Resource*>& bindings, GDX12Descriptor& materials,
	            const std::vector<GeometryTarget>& targets, bool viewsOnly = false,
	            const std::function<void()>& afterInteraction = {}, const std::function<void()>& afterDepth = {},
	            const std::function<void()>& afterWind = {});
	bool Record(UINT slot, Id, ID3D12GraphicsCommandList*) const;

  private:
	struct RemoteWind;
	void RetireRemoteWind(Executor&);
	static void AppendWindJobs(GDX12GrassRenderer&, Graph&, std::vector<ID3D12Resource*>&, NativeWork&, bool remote);
	void AppendWindImport(Graph&, std::vector<ID3D12Resource*>&, NativeWork&);
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
		std::unique_ptr<GDX12UploadBuffer<FragmentPose>> LindenResponse;
		NativeWork Work;
	};
	GDX12DeviceResources& _resources;
	BistroGrassGeometry _scene;
	std::unique_ptr<GDX12GrassRenderer> _gpu;
	std::unique_ptr<RemoteWind> _remoteWind;
	int _failedWindAdapter = -1;
	LindenWindSample _lindenWind;
	bool _importLindenResponse = false;
	std::vector<RecordedFrame> _frames;
	std::unique_ptr<GDX12RootSignature> _geometryRoot, _hiZRoot;
	ComPtr<ID3D12PipelineState> _buildGeometry, _cullGeometry, _clearGeometry, _buildHiZ;
};
} // namespace BistroRender

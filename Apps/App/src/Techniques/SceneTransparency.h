#pragma once
#include "NativeWork.h"
#include "SceneGeometry.h"
#include "SceneEnvironment.h"
#include "SceneParticles.h"
#include "Techniques/Water/SceneWater.h"
#include "Engine.RendererDX12/RenderPasses/GDX12WBOITCompositionPass.h"

namespace BistroRender
{
class SceneLighting;
class SceneTransparency
{
  public:
	SceneTransparency(GDX12DeviceResources&, RenderPipelineCommonData&);
	~SceneTransparency();
	Id Begin(const std::shared_ptr<QueueSubmission>&, SceneLighting&, UINT slot, uint64_t frame);
	void ExportPrepassDepth(Graph&, std::vector<ID3D12Resource*>&, GDX12Texture*);
	void AddExchange(const Schedule&, std::vector<Executor::PacketExchange>&);
	void Submitted(const std::vector<CompletionToken>&);
	void Presented();
	SceneWater& Water() { return _water; }
	void SetWaterRetirement(std::function<void()> retire) { _water.SetRetirement(std::move(retire)); }
	SceneParticles& Particles() { return _particles; }
	GDX12Texture* Append(Graph&, std::vector<ID3D12Resource*>&, SceneGeometry&, UINT slot, GDX12Texture* hdr,
	                     GDX12Texture* sun, GDX12Texture* local, SceneEnvironment&, SceneReflections&);
	using OpaqueContinuation = std::function<GDX12Texture*(Graph&, std::vector<ID3D12Resource*>&, GDX12Texture*,
	                                                       GDX12Texture*, GDX12Texture*)>;
	void SetOpaqueContinuation(OpaqueContinuation append, std::function<GDX12Texture*()> depth,
	                           std::function<GDX12Texture*()> motion, std::function<D3D12_CPU_DESCRIPTOR_HANDLE()> dsv)
	{
		_opaqueContinuation = std::move(append);
		_depth = std::move(depth);
		_motion = std::move(motion);
		_dsv = std::move(dsv);
	}
	bool Record(Id id, ID3D12GraphicsCommandList* cmd) const
	{
		return RecordPrepass(id, cmd) || _work.Record(id, cmd) || _water.Record(id, cmd) || _particles.Record(id, cmd);
	}
	GDX12Texture* ParticleMotion() const { return _particles.MotionOutput(); }
	GDX12Texture* Depth() const { return _depth ? _depth() : _water.Depth(); }
	GDX12Texture* Motion() const { return _motion ? _motion() : _water.Motion(); }

  private:
	bool RecordPrepass(Id, ID3D12GraphicsCommandList*) const;
	struct Remote;
	struct Shared;
	std::shared_ptr<Shared> _shared;
	std::vector<std::unique_ptr<Remote>> _remoteSlots;
	Remote* _remote = nullptr;
	SceneLighting* _lighting = nullptr;
	uint64_t _frame = 0;
	struct Frame
	{
		GDX12WBOITTransparencyPass Draw;
		GDX12WBOITCompositionPass Compose;
		GDX12Texture ImportedLayer;
		GDX12WBOITCompositionPass Apply;
		bool Initialized = false;
	};
	GDX12Texture* ApplyRemote(Graph&, std::vector<ID3D12Resource*>&, SceneGeometry&, UINT, Frame&, GDX12Texture*, SceneEnvironment&);
	void AppendRemote(Graph&, std::vector<ID3D12Resource*>&, SceneGeometry&, UINT, Frame&, GDX12Texture*,
	                           GDX12Texture*, GDX12Texture*, SceneEnvironment&, SceneReflections&);
	GDX12Texture* AppendPhase(Graph&, std::vector<ID3D12Resource*>&, SceneGeometry&, UINT slot, Frame&,
	                          GDX12Texture* hdr, GDX12Texture* sun, GDX12Texture* local, SceneEnvironment&,
	                          SceneReflections&);
	GDX12DeviceResources& _resources;
	RenderPipelineCommonData& _common;
	std::vector<Frame> _frames;
	NativeWork _work;
	SceneParticles _particles;
	SceneWater _water;
	OpaqueContinuation _opaqueContinuation;
	std::function<GDX12Texture*()> _depth, _motion;
	std::function<D3D12_CPU_DESCRIPTOR_HANDLE()> _dsv;
};
} // namespace BistroRender

#pragma once
#include "NativeWork.h"
#include "PublishedShadowMirror.h"
#include "SceneGeometry.h"
#include "SceneEnvironment.h"
#include "SceneParticles.h"
#include "Techniques/Water/SceneWater.h"
#include "Engine.RendererDX12/RenderPasses/GDX12WBOITCompositionPass.h"

namespace BistroRender
{
class SceneTransparency
{
  public:
	SceneTransparency(GDX12DeviceResources&, RenderPipelineCommonData&);
	~SceneTransparency();
	Id RequestedOwner(const std::shared_ptr<QueueSubmission>&, UINT slot);
	Id Begin(const std::shared_ptr<QueueSubmission>&, UINT slot, uint64_t frame, Id owner, const PublishedShadows&);
	bool ShadowBudgetFallback() const { return _shadowBudgetFallback; }
	void PrepareOpaqueDepth(Graph&, std::vector<ID3D12Resource*>&, UINT slot, GDX12Texture*);
	void AddExchange(const Schedule&, std::vector<Executor::PacketExchange>&);
	void Submitted(const std::vector<CompletionToken>&);
	void Presented();
	void RetireLocalShadows();
	SceneWater& Water() { return _water; }
	void SetWaterRetirement(std::function<void()> retire) { _water.SetRetirement(std::move(retire)); }
	SceneParticles& Particles() { return _particles; }
	GDX12Texture* Append(Graph&, std::vector<ID3D12Resource*>&, SceneGeometry&, UINT slot, GDX12Texture* hdr, SceneEnvironment&, const PublishedShadows&);
	using OpaqueContinuation = std::function<GDX12Texture*(Graph&, std::vector<ID3D12Resource*>&, GDX12Texture*,
	                                                       GDX12Texture*, GDX12Texture*)>;
	void SetOpaqueContinuation(OpaqueContinuation append, std::function<GDX12Texture*()> depth,
	                           std::function<GDX12Texture*()> motion)
	{
		_opaqueContinuation = std::move(append);
		_depth = std::move(depth);
		_motion = std::move(motion);
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
	uint64_t _frame = 0;
	unsigned _fallbackReasons = 0;
	bool _visibleMesh = false, _shadowBudgetFallback = false;
	struct Frame
	{
		GDX12WBOITTransparencyPass Draw;
		GDX12WBOITCompositionPass Compose;
		GDX12Texture ImportedLayer;
		GDX12Texture LocalDepthSnapshot;
		GDX12Texture* RasterDepth = nullptr;
		GDX12WBOITCompositionPass Apply;
		bool Initialized = false;
	};
	GDX12Texture* ApplyRemote(Graph&, std::vector<ID3D12Resource*>&, SceneGeometry&, UINT, Frame&, GDX12Texture*, SceneEnvironment&);
	void AppendRemote(Graph&, std::vector<ID3D12Resource*>&, SceneGeometry&, UINT, Frame&, GDX12Texture*,
	                  const PublishedShadows&);
	GDX12Texture* AppendPhase(Graph&, std::vector<ID3D12Resource*>&, SceneGeometry&, UINT slot, Frame&,
	                          GDX12Texture* hdr, SceneEnvironment&, const PublishedShadows&);
	GDX12DeviceResources& _resources;
	RenderPipelineCommonData& _common;
	std::vector<Frame> _frames;
	NativeWork _work;
	SceneParticles _particles;
	SceneWater _water;
	OpaqueContinuation _opaqueContinuation;
	std::function<GDX12Texture*()> _depth, _motion;
};
} // namespace BistroRender

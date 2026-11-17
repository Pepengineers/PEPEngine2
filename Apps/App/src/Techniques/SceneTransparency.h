#pragma once
#include "NativeWork.h"
#include "SceneGeometry.h"
#include "SceneEnvironment.h"
#include "SceneParticles.h"
#include "SceneWater.h"
#include "Engine.RendererDX12/RenderPasses/GDX12WBOITCompositionPass.h"

namespace Meta
{
class SceneTransparency
{
  public:
	SceneTransparency(GDX12DeviceResources &, RenderPipelineCommonData &);
	~SceneTransparency();
	void PrepareWater(Graph &, std::vector<ID3D12Resource *> &, SceneGeometry &, UINT slot);
	void SetWaterRetirement(std::function<void()> retire) { _water.SetRetirement(std::move(retire)); }
	GDX12Texture *Append(Graph &, std::vector<ID3D12Resource *> &, SceneGeometry &, UINT slot, GDX12Texture *hdr,
	                     GDX12Texture *sun, GDX12Texture *local, SceneEnvironment &);
	using OpaqueContinuation = std::function<GDX12Texture *(Graph &, std::vector<ID3D12Resource *> &, GDX12Texture *,
	                                                        GDX12Texture *, GDX12Texture *)>;
	void SetOpaqueContinuation(OpaqueContinuation append, std::function<GDX12Texture *()> depth,
	                           std::function<GDX12Texture *()> motion, std::function<D3D12_CPU_DESCRIPTOR_HANDLE()> dsv)
	{
		_opaqueContinuation = std::move(append);
		_depth = std::move(depth);
		_motion = std::move(motion);
		_dsv = std::move(dsv);
	}
	bool Record(Id id, ID3D12GraphicsCommandList *cmd) const
	{
		return _work.Record(id, cmd) || _water.Record(id, cmd) || _particles.Record(id, cmd);
	}
	GDX12Texture *ParticleMotion() const { return _particles.MotionOutput(); }
	GDX12Texture *Depth() const { return _depth ? _depth() : _water.Depth(); }
	GDX12Texture *Motion() const { return _motion ? _motion() : _water.Motion(); }

  private:
	struct Frame
	{
		GDX12WBOITTransparencyPass Draw;
		GDX12WBOITCompositionPass Compose;
		bool Initialized = false;
	};
	GDX12Texture *AppendPhase(Graph &, std::vector<ID3D12Resource *> &, SceneGeometry &, UINT slot, Frame &,
	                          GDX12Texture *hdr, GDX12Texture *sun, GDX12Texture *local, SceneEnvironment &);
	GDX12DeviceResources &_resources;
	RenderPipelineCommonData &_common;
	std::vector<Frame> _frames;
	NativeWork _work;
	SceneParticles _particles;
	SceneWater _water;
	OpaqueContinuation _opaqueContinuation;
	std::function<GDX12Texture *()> _depth, _motion;
	std::function<D3D12_CPU_DESCRIPTOR_HANDLE()> _dsv;
};
} // namespace Meta

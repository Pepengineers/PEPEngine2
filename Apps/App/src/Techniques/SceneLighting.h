#pragma once
#include "SceneAO.h"
#include "SceneReflections.h"
#include "SceneSunShadows.h"
#include "SceneLocalShadows.h"
#include "SceneTransparency.h"
#include "SceneProbePreview.h"
#include "Engine.RendererDX12/RenderPasses/GDX12DeferredLightingPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12SSSRPass.h"
namespace BistroRender
{
class SceneLighting
{
  public:
	SceneLighting(GDX12DeviceResources&, RenderPipelineCommonData&);
	~SceneLighting();
	void Freeze(SceneGeometry&, UINT slot, const std::shared_ptr<QueueSubmission>&, Id aoDevice);
	Id AOExecutionDevice() const { return _frames.at(_slot).AO->ExecutionDevice(); }
	std::optional<Executor::PacketExchange> AOExchange(const Schedule& schedule)
	{
		return _frames.at(_slot).AO->Exchange(schedule, _aoHistory);
	}
	void AOSubmitted(const std::vector<CompletionToken>& completions)
	{
		_frames.at(_slot).AO->Submitted(completions);
	}
	void AddProbeExchanges(const Schedule& schedule, std::vector<PacketWait>& waits,
	                       std::vector<Executor::PacketExchange>& exchanges)
	{
		_reflections->AddExchanges(schedule, waits, exchanges);
	}
	void ProbesSubmitted(const std::vector<CompletionToken>& completions)
	{
		_reflections->RemoteSubmitted(completions);
	}
	void AOPresented(uint64_t frame, const std::vector<CompletionToken>& completions)
	{
		_frames.at(_slot).AO->Presented(frame, completions);
	}
	void AppendEarlyAO(Graph& graph, std::vector<ID3D12Resource*>& bindings, Id ready)
	{
		_frames.at(_slot).AO->AppendEarlyInputs(graph, bindings, ready);
	}
	void AppendGpuLightGrid(Graph&, std::vector<ID3D12Resource*>&, const SceneGeometry&);
	void AppendAO(Graph&, std::vector<ID3D12Resource*>&);
	void Append(Graph&, std::vector<ID3D12Resource*>&, SceneGeometry&, SceneEnvironment&);
	using HDRContinuation = std::function<void(Graph&, std::vector<ID3D12Resource*>&, GDX12Texture*)>;
	void SetHDRContinuation(HDRContinuation continuation) { _hdrContinuation = std::move(continuation); }
	bool Record(Id, ID3D12GraphicsCommandList*) const;
	void Submitted(uint64_t fence);
	void Reset();
	SceneParticles& Particles() { return _transparency->Particles(); }
	SceneWater& Water() { return _transparency->Water(); }
	GDX12Texture* ParticleMotion() const { return _transparency->ParticleMotion(); }
	GDX12Texture* PostDepth() const { return _transparency->Depth(); }
	GDX12Texture* PostMotion() const { return _transparency->Motion(); }
	GDX12Texture* AOTexture(UINT slot) const { return _frames.at(slot).AO->Output(); }
	GDX12Texture* ReprojectedAOTexture(UINT slot) const
	{
		const auto& ao = _frames.at(slot).AO;
		return ao->UsesHistory() ? ao->Output() : nullptr;
	}
	GDX12Texture* DeferredHDR() const { return _deferred._color.get(); }
	GDX12Texture* SunShadow(UINT slot) const { return _sunShadows->Output(slot); }
	void SetWaterRetirement(std::function<void()> retire) { _transparency->SetWaterRetirement(std::move(retire)); }
	std::pair<size_t, size_t> Readiness() const
	{
		return _pendingReset ? std::pair<size_t, size_t>{} : _reflections->Readiness();
	}

  private:
	struct Frame
	{
		std::unique_ptr<SceneAO> AO;
		UINT Width = 0, Height = 0;
		uint64_t LightRevision = 0;
		GDX12ResourceHandle LightSource;
		bool UploadLights = false;
		GDX12LightGridConstants GridLayout{};
	};
	void AppendShadows(Graph&, std::vector<ID3D12Resource*>&, SceneGeometry&, SceneEnvironment&);
	void AppendDeferred(Graph&, std::vector<ID3D12Resource*>&, SceneGeometry&, SceneEnvironment&);
	GDX12Texture* AppendSSR(Graph&, std::vector<ID3D12Resource*>&, SceneGeometry&, SceneEnvironment&);
	void AppendComposition(Graph&, std::vector<ID3D12Resource*>&, SceneGeometry&, SceneEnvironment&, GDX12Texture*);
	void PrepareDeferred(SceneGeometry&, SceneEnvironment&);
	HDRContinuation _hdrContinuation;
	GDX12DeviceResources& _resources;
	RenderPipelineCommonData& _common;
	std::unique_ptr<SceneReflections> _reflections;
	std::unique_ptr<SceneProbePreview> _probePreview;
	std::unique_ptr<SceneLocalShadows> _probeStaticShadows;
	std::unique_ptr<SceneSunShadows> _sunShadows;
	std::unique_ptr<SceneLocalShadows> _localShadows;
	std::unique_ptr<SceneTransparency> _transparency;

	std::vector<Frame> _frames;
	GDX12DeferredLightingPass _deferred;
	std::unique_ptr<GDX12SSSRPass> _ssr;
	NativeWork _work;
	std::unique_ptr<GDX12RootSignature> _gridRoot;
	ComPtr<ID3D12PipelineState> _gridBuild;
	bool _forceReset = true;
	UINT _slot = 0;
	bool _initialized = false;
	bool _pendingReset = false;
	GDX12SSRSettings _ssrSettings{};
	GDX12LightGridSettings _gridSettings{};
	UINT _aoTemporalIndex = 0;
	SceneAO::History _aoHistory;
};
} // namespace BistroRender

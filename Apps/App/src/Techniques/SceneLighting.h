#pragma once
#include "SceneAO.h"
#include "SceneReflections.h"
#include "SceneSunShadows.h"
#include "SceneLocalShadows.h"
#include "SceneTransparency.h"
#include "SceneProbePreview.h"
#include "Engine.RendererDX12/RenderPasses/GDX12DeferredLightingPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12SSSRPass.h"
#include <map>
#include <chrono>
namespace BistroRender
{
class SceneLighting
{
  public:
	SceneLighting(GDX12DeviceResources&, RenderPipelineCommonData&);
	~SceneLighting();
	void Freeze(SceneGeometry&, UINT slot, const std::shared_ptr<QueueSubmission>&, Id aoDevice, Id transparencyOwner);
	void RetireShadowSkinInputs(UINT slot);
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
		AddShadowExchanges(schedule, exchanges);
	}
	void ProbesSubmitted(const std::vector<CompletionToken>& completions)
	{
		_reflections->RemoteSubmitted(completions);
		ShadowImportsSubmitted(completions);
	}
	void AOPresented(uint64_t frame, const std::vector<CompletionToken>& completions)
	{
		_frames.at(_slot).AO->Presented(frame, completions);
	}
	void AppendEarlyAO(Graph& graph, std::vector<ID3D12Resource*>& bindings, Id ready)
	{
		_frames.at(_slot).AO->AppendEarlyInputs(graph, bindings, ready);
	}
	void UpdateLightGridOwner(Executor& primary);
	Id LightGridDevice() const;
	void AppendPanelOwners(std::vector<std::pair<std::string, Id>>&) const;
	Id SunShadowDevice() const { return _sunOwner; }
	void AppendPublishedProbes(std::vector<std::pair<std::string, Id>>& owners) const
	{
		_reflections->AppendPublishedOwners(owners);
	}
	void AppendBenchmarkShadowOwners(std::vector<std::pair<UINT, Id>>& owners) const;
	void DrainLightGrid();
	void AppendGpuLightGrid(Graph&, std::vector<ID3D12Resource*>&);
	void SubmitLightGrid(const Schedule&, std::vector<PacketWait>&);
	void LightGridPresented(uint64_t frame, const Schedule&, const std::vector<CompletionToken>&);
	void AppendAO(Graph&, std::vector<ID3D12Resource*>&);
	void Append(Graph&, std::vector<ID3D12Resource*>&, SceneGeometry&, SceneEnvironment&);
	using HDRContinuation = std::function<void(Graph&, std::vector<ID3D12Resource*>&, GDX12Texture*)>;
	void SetHDRContinuation(HDRContinuation continuation) { _hdrContinuation = std::move(continuation); }
	bool Record(Id, ID3D12GraphicsCommandList*) const;
	void Submitted(uint64_t fence);
	void Reset();
	void RetireLocalShadows();
	std::string LocalShadowDistribution() const { return _localShadows->Distribution(_slot); }
	void ReportLocalShadows(uint64_t frame);
	UINT64 LocalShadowTexels() const { return _localShadows->ShadowTexels(_slot); }
	SceneTransparency& Transparency() { return *_transparency; }
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
	struct RemoteShadows;
	struct ShadowPublication;
	struct ShadowVertexBaseline;
	struct LightGridKernel;
	struct RemoteLightGrid;
	struct ShadowLane
	{
		Id first;
		bool second;
		UINT Resolution = 0;
		auto Key() const { return std::tie(first, second, Resolution); }
		bool operator<(const ShadowLane& other) const { return Key() < other.Key(); }
		bool operator==(const ShadowLane& other) const { return Key() == other.Key(); }
	};
	struct Frame
	{
		std::unique_ptr<SceneAO> AO;
		UINT Width = 0, Height = 0;
		uint64_t LightRevision = 0;
		GDX12ResourceHandle LightSource;
		bool UploadLights = false;
		UINT LightCount = 0, GridWords = 0;
		GDX12LightGridConstants GridLayout{};
		std::map<ShadowLane, std::unique_ptr<RemoteShadows>> ShadowOwners;
		std::vector<std::shared_ptr<ShadowPublication>> ShadowReads;
		std::map<UINT, std::array<uint64_t, 6>> LocalShadowOrders;
		std::array<uint64_t, 4> SunShadowOrders{};
		uint64_t LocalShadowLayout = 0;
		GDX12ResourceHandle LatestSunAtlas;
		UINT CrossSlotShadows = 0;
		CompletionToken ShadowConsumer;
	};
	void AppendRemoteLocalShadows(Graph&, std::vector<ID3D12Resource*>&, SceneGeometry&);
	void AddShadowExchanges(const Schedule&, std::vector<Executor::PacketExchange>&);
	void ShadowImportsSubmitted(const std::vector<CompletionToken>&);
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
	std::unique_ptr<SceneLocalShadows> _staticShadows;
	std::unique_ptr<SceneSunShadows> _sunShadows;
	std::unique_ptr<SceneLocalShadows> _localShadows;
	std::unique_ptr<SceneTransparency> _transparency;

	GDX12AmbientOcclusionPass::Pipelines _aoPipelines;
	std::vector<Frame> _frames;
	std::map<Id, std::shared_ptr<ShadowVertexBaseline>> _shadowVertexBaselines;
	std::map<UINT, std::array<std::shared_ptr<ShadowPublication>, 6>> _latestLocalShadows;
	std::array<std::shared_ptr<ShadowPublication>, 4> _latestSunShadows;
	uint64_t _shadowOrder = 0;
	UINT _reportedCrossSlotShadows = 0;
	std::shared_ptr<QueueSubmission> _submissions;
	std::chrono::steady_clock::time_point _nextShadowReport{};
	std::string _lastShadowReport;
	UINT _lastShadowCeiling = UINT_MAX;
	GDX12DeferredLightingPass _deferred;
	std::unique_ptr<GDX12SSSRPass> _ssr;
	NativeWork _work;
	std::unique_ptr<LightGridKernel> _gridKernel;
	std::unique_ptr<RemoteLightGrid> _remoteGrid;
	bool _forceReset = true;
	UINT _slot = 0;
	Id _sunOwner = 0;
	UINT _sunRemoteMask = 0;
	int _sunSync = 1;
	uint64_t _localPlacementEpoch = 0, _sunPlacementEpoch = 0;
	std::unordered_set<Id> _reportedShadowSkinOwners;
	bool _initialized = false;
	bool _pendingReset = false;
	GDX12SSRSettings _ssrSettings{};
	GDX12LightGridSettings _gridSettings{};
	UINT _aoTemporalIndex = 0;
	SceneAO::History _aoHistory;
};
} // namespace BistroRender

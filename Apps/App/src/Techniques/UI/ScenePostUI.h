#pragma once
#include "Techniques/NativeWork.h"
#include "App.Base/Rendering/IRenderPipeline.h"
#include "Engine.RendererDX12/RenderPasses/GDX12DepthOfFieldPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12MotionBlurPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12ReconstructionPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12OutputToScreenPass.h"
#include "Engine.UI/OffscreenUI.h"
#include "Engine.UI/UIScreenComposite.h"
#include "BistroUIPosters.h"
#include "SceneTextureInspector.h"
#include <array>
#include <string_view>

namespace BistroRender
{
class ScenePostUI
{
  public:
	explicit ScenePostUI(RenderPipelineContext&);
	~ScenePostUI();
	GDX12ReconstructionPass& Reconstruction();
	std::vector<GDX12RenderPass*> Passes();
	bool UpdatePostOwner(RenderPipelineContext&, const std::shared_ptr<QueueSubmission>&, Id);
	using Devices = std::array<std::pair<std::string_view, Id>, 5>;
	Devices ExecutionDevices() const;
	std::array<uint64_t, 4> BenchmarkTransferBytes() const;
	std::pair<bool, bool> BenchmarkPostEffects() const;
	std::pair<Id, Id> BenchmarkPublication() const;
	const std::string& PostFallback() const { return _postFallback; }
	const std::string& ReconstructionFailure() const;
	Id PostDevice() const;
	CompletionToken ReconstructionFence(CompletionToken local) const;
	bool ReconstructionRecorded() const;
	void DrainPost();
	void AddPostExchange(const Schedule&, std::vector<Executor::PacketExchange>&);
	void PostPresented();
	bool NeedsResize() const;
	void BeginPlan(Executor&, uint64_t frame);
	void SubmitHud(const Schedule&, std::vector<PacketWait>&);
	void DrainHud();
	void HudSubmitted(const std::vector<CompletionToken>&);
	void HudPresented();
	struct HudLayer
	{
		Id Owner;
		GDX12ResourceHandle Texture;
		CompletionToken Ready;
	};
	// Pure game HUD for a secondary owner, in COMMON after Ready, valid through this primary frame slot.
	std::optional<HudLayer> OwnerHudLayer() const;
	void Submitted(UINT slot)
	{
		_exports.at(slot) = _pendingExports;
		_publishedInputs = _pendingPublished;
		if (!_remotePost) _reconstruction.Submitted();
	}
	void PrepareMaterials(Executor&, uint64_t frame, double animationSeconds, double hudSeconds, GDX12Texture* cloudImage);
	void AddPosterWaits(const Schedule& schedule, std::vector<PacketWait>& waits) { _posters->AddWaits(schedule, waits); }
	void PostersSubmitted(uint64_t frame, const std::vector<CompletionToken>& completions)
	{
		_posters->Submitted(frame, completions);
	}
	SceneTextureInspector& Textures() { return _inspector; }
	void AppendTextureReadback(Graph& graph, std::vector<ID3D12Resource*>& bindings, GDX12Texture& backBuffer)
	{
		_inspector.AppendReadback({_inspector, graph, bindings, _work, *_resources.Device,
		                           backBuffer.GetResource()->D3DResource.Get()}, _resources.CurrFrameConstantsIndex);
	}
	void ReleasePresentation();
	void Resize(); // Caller drains the executor and the presentation queue first.
	void Reset();
	void Append(Graph&, std::vector<ID3D12Resource*>&, GDX12Texture* hdr, GDX12Texture* depth, GDX12Texture* motion,
	            GDX12Texture* featureMotion, GDX12Texture* target,
	            const std::function<void(GDX12CommandList*)>& finish);
	std::vector<CanvasRead> AppendPosters(Graph&, std::vector<ID3D12Resource*>&);
	bool Record(Id id, ID3D12GraphicsCommandList* cmd) const { return _work.Record(id, cmd); }

  private:
	struct CapturedScreen;
	struct InputNeeds
	{
		bool DoF, Motion, CompleteMotion;
		bool Depth() const { return DoF || Motion; }
	};
	InputNeeds RequiredInputs(GDX12Texture& hdr);
	Id AppendFrameGeneration(Graph&, std::vector<ID3D12Resource*>&, GDX12Texture* depth, Id dependency);
	Id AppendReconstruction(Graph&, std::vector<ID3D12Resource*>&, GDX12Texture* hdr, GDX12Texture* depth,
	                        GDX12Texture* motion, GDX12Texture* featureMotion, GDX12Texture* target,
	                        bool completeMotionRequired, std::vector<Id> dependencies);
	void AppendScreen(Graph&, std::vector<ID3D12Resource*>&, GDX12Texture* target,
	                  const std::function<void(GDX12CommandList*)>& finish, Id dependency, const CapturedScreen&);
	Id Packet(Graph&, std::vector<ID3D12Resource*>&, const char*, std::vector<std::pair<GDX12Resource*, Mode>>,
	          const std::function<void(GDX12CommandList*)>&, std::vector<Id> dependencies = {},
	          GDX12Texture* renderTarget = nullptr, State textureState = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
	ScenePostUI(RenderPipelineContext&, bool sceneOwner);
	CapturedScreen CaptureScreen(Graph&, std::vector<ID3D12Resource*>&, ID3D12Resource*,
	                             const SceneTextureInspector::Input* post = nullptr);
	Id AppendStack(Graph&, std::vector<ID3D12Resource*>&, GDX12Texture* hdr, GDX12Texture* depth,
	               GDX12Texture* motion, GDX12Texture* featureMotion, GDX12Texture* target,
	               const InputNeeds&, std::vector<Id> = {});
	void AppendRemotePost(Graph&, std::vector<ID3D12Resource*>&, GDX12Texture*, GDX12Texture*, GDX12Texture*,
	                      GDX12Texture*, GDX12Texture*, const InputNeeds&, const std::function<void(GDX12CommandList*)>&);
	Id CompositeScreen(Graph&, std::vector<ID3D12Resource*>&, GDX12Texture*, GDX12Texture*, Id,
	                   const SceneTextureInspector::Atlases&, const std::shared_ptr<const Engine::UI::DrawData>&, bool);
	struct RemotePost;
	struct HudRaster;
	struct RemoteHud;
	void EnsureHudSource();
	HudRaster& LocalHud(GDX12DeviceResources& atlasSources);
	void PrepareRemoteHud(Id owner, Id display, UINT width, UINT height, const CapturedScreen&);

	struct HudFrame
	{
		std::unique_ptr<GDX12Texture> HealthBar;
		std::shared_ptr<const Engine::UI::DrawData> Draw;
		std::shared_ptr<const void> RasterOwner;
	};
	struct ScreenSlot
	{
		std::unique_ptr<GDX12Texture> Target;
		HudFrame Game;
		std::shared_ptr<const Engine::UI::DrawData> Draw;
		std::shared_ptr<const void> RasterOwner, CompositeOwner;
		std::shared_ptr<RemoteHud> HudOwner;
		Id HudImport = Invalid, HudComposite = Invalid;
	};
	GDX12DeviceResources& _resources;
	RenderPipelineCommonData& _common;
	GDX12SwapChain* _swapchain;
	RenderModule& _render;
	SceneTextureInspector _inspector;
	std::unique_ptr<ImDrawList> _gameOverlay;
	std::unique_ptr<HudRaster> _localHud;
	std::shared_ptr<RemoteHud> _remoteHud;
	std::shared_ptr<QueueSubmission> _hudSubmissions;
	uint64_t _hudFrame = 0;
	float _hudTime = 0;
	GDX12DepthOfFieldPass _dof;
	GDX12MotionBlurPass _blur;
	GDX12ReconstructionPass _reconstruction;
	GDX12OutputToScreenPass _output;
	std::unique_ptr<BistroUIPosters> _posters;
	std::unique_ptr<Engine::UI::OffscreenRenderer> _screen;
	std::unique_ptr<Engine::UI::ScreenComposite> _composite;
	std::vector<ScreenSlot> _slots;
	std::unique_ptr<RemotePost> _remotePost;
	Id _requestedPost = Invalid;
	GDX12ReconstructionSettings _postRequest{};
	std::string _postFallback, _reconstructionFailure;
	bool _reconstructionRecorded = false;
	int _postFgRequest = -1;
	NativeWork _work;
	std::vector<std::array<GDX12ResourceHandle, 2>> _exports;
	std::array<GDX12ResourceHandle, 2> _pendingExports;
	std::vector<GDX12ResourceHandle> _publishedInputs, _pendingPublished;
	bool _initialized = false, _resize = false, _outputInitialized = false;
};
} // namespace BistroRender

#include "ScenePostUI.h"
#include "BistroInfoPanel.h"
#include "BistroGameHUD.h"
#include "Techniques/ProbeInputMirror.h"
#include "App.Base/Modules/RenderModule.h"
#include "App.Base/Modules/SceneManagerModule.h"
#include "Engine.Core/BenchmarkEngine.h"
#include "Engine.Core/AssetManager.h"
#include "Engine.Core/Logging/Logger.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12Device.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include "Engine.RendererDX12/GDX12RootSignature.h"
#include "Engine.RendererDX12/GDX12ShaderCompiler.h"
#include "Engine.RendererDX12/GDX12SwapChain.h"
#include "Engine.RendererDX12/GDX12FrameGeneration.h"
#include "Engine.RendererDX12/GDX12MotionBlurSettings.h"
#include "Engine.RendererDX12/RenderPasses/GDX12TAAPass.h"
#include "Common/GameTimer.h"
#include <filesystem>
#include <unordered_set>

namespace BistroRender
{
namespace
{
AutoConsoleVariable<int> HudAdapter(L"App.UI.GameHUD.Adapter", -1,
    L"DXGI index for Bistro's game HUD. -1 renders locally.", EConsoleVariableFlags::Default,
    [](int index) { return index >= -1; });

constexpr char HudAtlasName[] = "BistroDemoHUD";

Id HudPacket(const Schedule& schedule, Id operation)
{
	const auto it = std::find_if(schedule.Packets.begin(), schedule.Packets.end(),
	    [operation](const auto& packet) { return packet.Operation == operation; });
	if (it == schedule.Packets.end()) throw std::logic_error("Missing game HUD packet");
	return Id(it - schedule.Packets.begin());
}

ComPtr<ID3D12PipelineState> CreateHealthBarPipeline(GDX12Device& device, GDX12RootSignature& root)
{
	auto& compiler = GDX12ShaderCompiler::GetInstance();
	const auto vs = compiler.CompileShader(&device, APP_SHADERS_FOLDER L"BistroHealthBar.hlsl", nullptr, "VSMain", "vs");
	const auto ps = compiler.CompileShader(&device, APP_SHADERS_FOLDER L"BistroHealthBar.hlsl", nullptr, "PSMain", "ps");
	D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
	desc.pRootSignature = root.GetRootSignature().Get();
	desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
	desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
	desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	desc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	desc.DepthStencilState.DepthEnable = FALSE;
	desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	auto& blend = desc.BlendState.RenderTarget[0];
	blend.BlendEnable = TRUE;
	blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
	blend.SrcBlendAlpha = D3D12_BLEND_ONE;
	blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
	desc.SampleMask = UINT_MAX;
	desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	desc.NumRenderTargets = 1;
	desc.RTVFormats[0] = Engine::UI::OffscreenFormat;
	desc.SampleDesc.Count = 1;
	ComPtr<ID3D12PipelineState> pipeline;
	ThrowIfFailed(device.GetDevice()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline)));
	return pipeline;
}
}

namespace
{
struct ScreenTransfer
{
	QueueSubmission::TransferBuffer Buffer;
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT Layout{};
	UINT64 Bytes = 0;
	Id From = Invalid, To = Invalid;

	void Prepare(const QueueSubmission& submit, Id from, Id to, const D3D12_RESOURCE_DESC& desc)
	{
		if (Buffer.Resources[0] && From == from && To == to && Layout.Footprint.Width == desc.Width &&
		    Layout.Footprint.Height == desc.Height && Layout.Footprint.Format == desc.Format) return;
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT target{};
		UINT64 targetBytes = 0;
		submit.Device(from).GetDevice()->GetCopyableFootprints(&desc, 0, 1, 0, &Layout, nullptr, nullptr, &Bytes);
		submit.Device(to).GetDevice()->GetCopyableFootprints(&desc, 0, 1, 0, &target, nullptr, nullptr, &targetBytes);
		if (!Bytes || Bytes == UINT64_MAX || Bytes != targetBytes || Layout.Offset != target.Offset ||
		    Layout.Footprint.RowPitch != target.Footprint.RowPitch || Layout.Footprint.Format != target.Footprint.Format ||
		    Layout.Footprint.Width != target.Footprint.Width || Layout.Footprint.Height != target.Footprint.Height ||
		    Layout.Footprint.Depth != target.Footprint.Depth)
			throw std::runtime_error("Screen transfer requires matching adapter footprints");
		Buffer = submit.CreateTransferBuffer(from, to, Bytes);
		From = from;
		To = to;
	}

	Id Copy(Graph& graph, std::vector<ID3D12Resource*>& bindings, NativeWork& work, const char* name, GDX12Texture& image)
	{
		const bool exporting = graph.Device == From;
		if (!exporting && graph.Device != To) throw std::logic_error("Screen transfer bound to the wrong device");
		const CD3DX12_TEXTURE_COPY_LOCATION tex(image.GetResource()->D3DResource.Get(), 0u);
		const CD3DX12_TEXTURE_COPY_LOCATION shared(Buffer.Resources[exporting ? 0 : 1].Get(), Layout);
		const auto source = exporting ? tex : shared;
		const auto dest = exporting ? shared : tex;
		const auto read = NativeWork::ResourceFor(graph, bindings, source.pResource, name);
		const auto write = NativeWork::ResourceFor(graph, bindings, dest.pResource, name);
		return work.Add(graph, name,
		    {{read, Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE}, {write, Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST}},
		    [source, dest](auto* cmd) { cmd->CopyTextureRegion(&dest, 0, 0, 0, &source, nullptr); });
	}
};

void EnsureScreenTexture(std::unique_ptr<GDX12Texture>& image, GDX12DeviceResources& device, UINT width, UINT height)
{
	if (image && image->GetWidth() == width && image->GetHeight() == height) return;
	image = std::make_unique<GDX12Texture>();
	GDX12RenderHelpers::InitializeRenderTarget(*image, device, width, height, Engine::UI::OffscreenFormat, {0, 0, 0, 0});
}
}

struct ScenePostUI::CapturedScreen
{
	std::shared_ptr<const Engine::UI::DrawData> Overlay, Game;
	SceneTextureInspector::Atlases Previews{};
	GameHUD::Bar Bar{};
};

struct ScenePostUI::HudRaster
{
	explicit HudRaster(GDX12DeviceResources& resources, GDX12Texture& atlas) : Resources(resources), Atlas(&atlas) {}

	Id Append(Graph& graph, std::vector<ID3D12Resource*>& bindings, NativeWork& work, HudFrame& frame,
	          GDX12Texture& target, Engine::UI::OffscreenRenderer& renderer, const GameHUD::Bar& bar,
	          std::vector<Id> dependencies = {})
	{
		const auto id = [&](GDX12Texture& texture, const char* name) {
			return NativeWork::ResourceFor(graph, bindings, texture.GetResource()->D3DResource.Get(), name);
		};
		const State read = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
		const State renderTarget = D3D12_RESOURCE_STATE_RENDER_TARGET;
		auto* atlas = Atlas->GetResource();
		const Id atlasId = NativeWork::ImportReadOnlyTexture(graph, bindings, *atlas, read, "game HUD atlas");
		std::vector<Access> accesses{{id(target, "game HUD target"), Mode::Write, renderTarget},
		                            {atlasId, Mode::Read, read}};
		std::vector<Engine::UI::TextureBinding> textures{{GameHUD::Atlas, atlas->D3DResource, Atlas->GetFormat()}};
		if (bar.Sparks)
		{
			if (!Pipeline)
			{
				GDX12RootSignatureDesc root;
				root.Constants = {5};
				Root = std::make_unique<GDX12RootSignature>(Resources.Device, root);
				Pipeline = CreateHealthBarPipeline(*Resources.Device, *Root);
			}
			const UINT width = UINT(std::ceil(bar.Width)), height = UINT(std::ceil(bar.Height));
			if (!frame.HealthBar || frame.HealthBar->GetWidth() != width || frame.HealthBar->GetHeight() != height)
			{
				frame.HealthBar = std::make_unique<GDX12Texture>();
				GDX12RenderHelpers::InitializeRenderTarget(*frame.HealthBar, Resources, width, height,
				    Engine::UI::OffscreenFormat, {0, 0, 0, 0});
			}
			auto* barTarget = frame.HealthBar.get();
			const Id barId = id(*barTarget, "game HUD health bar");
			const Id sparks = work.Add(graph, "Game HUD health bar (GPU sparks)",
			    {{barId, Mode::Write, renderTarget}}, [this, bar, barTarget](auto* cmd)
			    {
				    const auto rtv = barTarget->GetRTV()->CPUHandle;
				    const auto viewport = barTarget->GetViewport();
				    const auto scissor = barTarget->GetScissorRect();
				    cmd->SetGraphicsRootSignature(Root->GetRootSignature().Get());
				    cmd->SetPipelineState(Pipeline.Get());
				    cmd->SetGraphicsRoot32BitConstants(Root->GetBRootParamIndex(0), 5, &bar, 0);
				    cmd->RSSetViewports(1, &viewport);
				    cmd->RSSetScissorRects(1, &scissor);
				    cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
				    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
				    cmd->DrawInstanced(3, 1 + bar.Sparks, 0, 0);
			    });
			dependencies.push_back(sparks);
			accesses.push_back({barId, Mode::Read, read});
			textures.push_back({GameHUD::HealthBar, barTarget->GetResource()->D3DResource, Engine::UI::OffscreenFormat});
		}
		return work.AddOwned(graph, "Game HUD raster", std::move(accesses), bindings,
		    [&frame, &target, &renderer, textures = std::move(textures)](auto* cmd)
		    {
			    frame.RasterOwner = renderer.TryRecord(*cmd, target.GetResource()->D3DResource, frame.Draw, textures);
			    if (!frame.RasterOwner) throw std::logic_error("Game HUD backend slot is still in flight");
		    }, "Game HUD cannot submit private queue signals", std::move(dependencies));
	}

	GDX12DeviceResources& Resources;
	GDX12Texture* Atlas;
	uint64_t AtlasRevision = 0;
	std::unique_ptr<GDX12RootSignature> Root;
	ComPtr<ID3D12PipelineState> Pipeline;
};

struct ScenePostUI::RemoteHud
{
	struct Frame
	{
		HudFrame Game;
		std::unique_ptr<GDX12Texture> Layer;
		ScreenTransfer Transfer;
		Graph Plan;
		NativeWork Work;
		std::vector<ID3D12Resource*> Bindings;
		Id Raster = Invalid;
		CompletionToken RasterReady, Ready, Imported, Applied, Completed;
		UINT64 Number = 0;
		bool Overlay = false;
	};

	RemoteHud(const std::shared_ptr<QueueSubmission>& submit, Id owner, UINT slots)
	    : Owner(owner), GPU(submit, owner, slots, 1), Frames(slots),
	      Renderer(submit->Device(owner), slots, true)
	{
		auto& device = submit->Device(owner);
		Resources.Device = &device;
		Resources.SRV_UAV_Heap = std::make_shared<GDX12DescriptorHeap>(&device,
		    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, Texture2D_StartIndex + 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
		Resources.RTVHeap = std::make_unique<GDX12DescriptorHeap>(&device,
		    D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2 * slots, D3D12_DESCRIPTOR_HEAP_FLAG_NONE);
	}

	static void Retire(RemoteHud* owner) noexcept
	{
		try { owner->GPU.Drain(); }
		catch (...)
		{
			// A quarantined queue can still reference frame uploads, textures and ImGui descriptors.
			return;
		}
		delete owner;
	}

	void RefreshAtlas(const GDX12DeviceResources::TextureReplicaLoader& source)
	{
		if (Raster && AtlasRevision == source.Revision) return;
		auto* atlas = source.Load(Resources);
		// Each recorded draw retains its own native texture handle through slot retirement.
		if (Raster) Raster->Atlas = atlas;
		else Raster = std::make_unique<HudRaster>(Resources, *atlas);
		AtlasRevision = source.Revision;
	}

	void PrepareLayer(Frame& frame, Id display, UINT width, UINT height)
	{
		EnsureScreenTexture(frame.Layer, Resources, width, height);
		frame.Transfer.Prepare(*GPU.Submissions(), Owner, display, frame.Layer->GetResource()->D3DResource->GetDesc());
	}

	void Report(const Frame& frame)
	{
		const auto& submit = *GPU.Submissions();
		if (!frame.Applied || !frame.Completed || !submit.Complete(frame.Completed) ||
		    (Reported && Width == frame.Layer->GetWidth() && Height == frame.Layer->GetHeight())) return;
		Reported = true;
		Width = frame.Layer->GetWidth();
		Height = frame.Layer->GetHeight();
		Log::Info("RenderGraph", "Game HUD Exact completed frame=", frame.Number, " owner=", submit.AdapterLabel(Owner),
		    " display=", submit.AdapterLabel(0), " dimensions=", Width, "x", Height,
		    " format=RGBA8_UNORM premultiplied=1 sceneInputBytes=0 resultBytes=", frame.Transfer.Bytes,
		    " rowPitch=", frame.Transfer.Layout.Footprint.RowPitch, " ownerRasterFence=", frame.RasterReady.Value,
		    " ownerExportFence=", frame.Ready.Value, " displayImportFence=", frame.Imported.Value,
		    " displayCompositeFence=", frame.Applied.Value, " completedFrameFence=", frame.Completed.Value,
		    " infoDebugAbove=", frame.Overlay);
	}

	Id Owner;
	Executor GPU;
	GDX12DeviceResources Resources;
	std::vector<Frame> Frames;
	Engine::UI::OffscreenRenderer Renderer;
	std::unique_ptr<HudRaster> Raster;
	uint64_t AtlasRevision = 0;
	bool Reported = false;
	UINT Width = 0, Height = 0;
};

struct ScenePostUI::RemotePost
{
	struct Frame
	{
		std::unique_ptr<ProbeInputMirror> Inputs;
		std::array<GDX12Texture, 4> Views;
		std::unique_ptr<GDX12Texture> Preview, ImportedHud, Output;
		ScreenTransfer Hud, Inspector, Display;
		Graph Plan;
		std::vector<ID3D12Resource*> Bindings;
		Id Export = Invalid, Import = Invalid, SceneImport = Invalid, HudImport = Invalid, InspectorImport = Invalid;
		bool PeerHud = false, Presented = false;
		CompletionToken SceneReady, Produced;
		std::array<UINT64, 4> Bytes{};
		UINT64 Number = 0, BundleBytes = 0, HudBytes = 0, UiBytes = 0;
		Id HudOwner = Invalid;
	};

	RemotePost(RenderPipelineContext& c, const std::shared_ptr<QueueSubmission>& submit, Id id)
	    : Owner(id), GPU(submit, id, UINT(c.Primary.FrameConstants.size()), 1), Frames(c.Primary.FrameConstants.size())
	{
		Resources.Initialize(&submit->Device(id));
		Resources.SRV_UAV_Heap->UseTransientDescriptorTail();
		RenderPipelineContext context{c.Runtime, Resources, c.Common, c.BackBuffer};
		Stack.reset(new ScenePostUI(context, false));
		for (auto& frame : Frames) frame.Inputs = std::make_unique<ProbeInputMirror>(*submit, id);
	}

	Id Owner;
	Executor GPU;
	GDX12DeviceResources Resources;
	std::unique_ptr<ScenePostUI> Stack;
	std::vector<Frame> Frames;
	bool Direct = false, Reported = false;
};

GDX12ReconstructionPass& ScenePostUI::Reconstruction()
{
	return _remotePost ? _remotePost->Stack->_reconstruction : _reconstruction;
}

std::vector<std::pair<std::string, Id>> ScenePostUI::ExecutionDevices() const
{
	const auto slot = _resources.CurrFrameConstantsIndex;
	const auto& screen = _slots.at(slot);
	const Id post = _remotePost ? _remotePost->Owner : 0;
	const Id presentation = _remotePost && _remotePost->Direct ? post : 0;
	const Id hud = _remotePost ? _remotePost->Frames.at(slot).HudOwner :
	    screen.Game.Draw ? (screen.HudOwner ? screen.HudOwner->Owner : 0) : Invalid;
	const auto posters = _posters->ExecutionDevices();
	return {{"PostAdapter", post}, {"PresentationAdapter", presentation}, {"GameHUDAdapter", hud},
	    {"AtriumCanvasAdapter", posters[0]}, {"TerraceCanvasAdapter", posters[1]}, {"CloudCanvasAdapter", posters[2]}};
}

std::vector<GDX12RenderPass*> ScenePostUI::Passes()
{
	if (_remotePost) return _remotePost->Stack->Passes();
	return {&_dof, &_blur, &_reconstruction, &_output};
}

void ScenePostUI::DrainPost()
{
	if (!_remotePost) return;
	auto& owner = *_remotePost;
	owner.GPU.Drain();
	for (UINT slot = 0; slot < owner.Frames.size(); ++slot)
		_inspector.RetireReadback(owner.Owner, slot);
}

bool ScenePostUI::UpdatePostOwner(RenderPipelineContext& c, const std::shared_ptr<QueueSubmission>& submit, Id owner)
{
	const auto settings = GetReconstructionSettings();
	const auto fg = GetFrameGenerationProvider();
	if (_requestedPost == Invalid && !owner && settings == _reconstruction.GetStatus().Requested)
	{
		_requestedPost = 0;
		_postRequest = settings;
		_postFgRequest = int(fg);
		return false;
	}
	const bool sameRequest = _requestedPost == owner && _postRequest == settings && _postFgRequest == int(fg);
	std::string reason;
	if (sameRequest)
	{
		if (!_remotePost || !_remotePost->Direct || fg == GDX12FrameGenerationProvider::Off) return false;
		const auto& active = c.BackBuffer->GetFrameGeneration().GetStatus();
		if (active.Requested != fg || active.Effective == fg) return false;
		reason = "lost active FG on post adapter: " + active.FallbackReason;
	}
	_requestedPost = owner;
	_postRequest = settings;
	_postFgRequest = int(fg);
	DrainHud();
	c.Runtime.DrainFrameWork();
	auto retired = std::move(_remotePost);
	if (owner && !sameRequest)
	{
		auto remote = std::make_unique<RemotePost>(c, submit, owner);
		const auto& reconstruction = remote->Stack->_reconstruction.GetStatus();
		if (reconstruction.Effective != settings.Provider)
			reason = "requested SR on post adapter: " + reconstruction.FallbackReason;
		else
		{
			try
			{
				c.SetPresentationDevice(*remote->Resources.Device);
				remote->Direct = true;
			}
			catch (const DxException& failure)
			{
				if (!c.BackBuffer) throw;
				reason = WStringToString(failure.ToString());
			}
			catch (const std::exception& failure)
			{
				if (!c.BackBuffer) throw;
				reason = failure.what();
			}
			if (fg == GDX12FrameGenerationProvider::Off ||
			    (remote->Direct && c.BackBuffer->GetFrameGeneration().GetStatus().Effective == fg))
			{
				remote->Stack->_swapchain = c.BackBuffer;
				_remotePost = std::move(remote);
			}
			else if (remote->Direct)
				reason = "requested FG on post adapter: " + c.BackBuffer->GetFrameGeneration().GetStatus().FallbackReason;
			if (remote && remote->Direct)
				c.SetPresentationDevice(*c.Primary.Device);
		}
	}
	if (!_remotePost)
	{
		c.SetPresentationDevice(*c.Primary.Device);
		_reconstruction.ApplySettings();
		_reconstruction.QueryRenderTargetResolution();
		_common.Upscaler = &_reconstruction;
	}
	_swapchain = c.BackBuffer;
	_common.RequestHistoryReset = _common.ResetUpscalerHistory = true;
	Log::Info("RenderGraph", "Bistro post placement scene=", submit->AdapterLabel(0),
	    " post=", submit->AdapterLabel(_remotePost ? owner : 0),
	    " presentation=", submit->AdapterLabel(_remotePost && _remotePost->Direct ? owner : 0),
	    " reason=", reason.empty() ? "requested path" : reason);
	return true;
}

void ScenePostUI::BeginPlan(Executor& primary, uint64_t frame)
{
	HudPresented();
	_hudSubmissions = primary.Submissions();
	_hudFrame = frame;
	_inspector.BeginPlan();
	_work.Commands.clear();
	_pendingExports = {};
	_pendingPublished = _publishedInputs;
}

void ScenePostUI::SubmitHud(const Schedule& primary, std::vector<PacketWait>& waits)
{
	const UINT slot = _resources.CurrFrameConstantsIndex;
	auto& screen = _slots.at(slot);
	if (!screen.HudOwner) return;
	auto& owner = *screen.HudOwner;
	auto& frame = owner.Frames.at(slot);
	const auto schedule = Compile(frame.Plan);
	const Id raster = HudPacket(schedule, frame.Raster);
	const bool displayOnScene = screen.HudImport != Invalid;
	if (displayOnScene)
	{
		screen.HudImport = HudPacket(primary, screen.HudImport);
		screen.HudComposite = HudPacket(primary, screen.HudComposite);
	}
	owner.GPU.Reserve(schedule);
	owner.GPU.Execute(schedule, frame.Bindings, [&](Id op, UINT, auto* cmd) {
		if (!frame.Work.Record(op, cmd)) throw std::logic_error("Missing owner game HUD work");
	});
	frame.RasterReady = owner.GPU.Completions().at(raster);
	frame.Ready = owner.GPU.Completions().back();
	if (displayOnScene) waits.push_back({screen.HudImport, frame.Ready});
}

void ScenePostUI::DrainHud()
{
	std::unordered_set<RemoteHud*> owners;
	if (_remoteHud) owners.insert(_remoteHud.get());
	for (const auto& screen : _slots)
		if (screen.HudOwner) owners.insert(screen.HudOwner.get());
	for (auto* owner : owners) owner->GPU.Drain();
}

void ScenePostUI::HudSubmitted(const std::vector<CompletionToken>& completions)
{
	const UINT slot = _resources.CurrFrameConstantsIndex;
	auto& screen = _slots.at(slot);
	if (!screen.HudOwner || screen.HudImport == Invalid || completions.empty()) return;
	auto& frame = screen.HudOwner->Frames.at(slot);
	frame.Imported = completions.at(screen.HudImport);
	frame.Applied = completions.at(screen.HudComposite);
	frame.Completed = completions.back();
}

void ScenePostUI::HudPresented()
{
	const UINT slot = _resources.CurrFrameConstantsIndex;
	auto& screen = _slots.at(slot);
	if (screen.HudOwner) screen.HudOwner->Report(screen.HudOwner->Frames.at(slot));
}

std::optional<ScenePostUI::HudLayer> ScenePostUI::OwnerHudLayer() const
{
	const UINT slot = _resources.CurrFrameConstantsIndex;
	const auto& screen = _slots.at(slot);
	if (!screen.HudOwner) return std::nullopt;
	const auto& frame = screen.HudOwner->Frames.at(slot);
	if (!frame.Ready) return std::nullopt;
	return HudLayer{screen.HudOwner->Owner, frame.Layer->GetResource()->D3DResource, frame.Ready};
}

ScenePostUI::ScenePostUI(RenderPipelineContext& c) : ScenePostUI(c, true) {}

ScenePostUI::ScenePostUI(RenderPipelineContext& c, bool sceneOwner)
    : _resources(c.Primary), _common(c.Common), _swapchain(c.BackBuffer), _render(c.Runtime),
      _inspector(UINT(c.Primary.FrameConstants.size())),
      _slots(c.Primary.FrameConstants.size()),
      _exports(c.Primary.FrameConstants.size())
{
	_dof.Setup(&_resources, &_common);
	_blur.Setup(&_resources, &_common);
	_blur.SetPassthroughWhenDisabled(true);
	_reconstruction.Setup(&_resources, &_common);
	_output.Setup(&_resources, &_common);
	if (sceneOwner) _posters = std::make_unique<BistroUIPosters>(c.Runtime,
	                                             BenchmarkEngine::GetLocator().GetModule<SceneManagerModule>(), true);
}
ScenePostUI::~ScenePostUI()
{
	_dof.ClearDenendencies();
	_blur.ClearDenendencies();
	_reconstruction.ClearDenendencies();
	_output.ClearDenendencies();
}
void ScenePostUI::ReleasePresentation()
{
	if (_remotePost) _remotePost->Stack->ReleasePresentation();
	_gameOverlay.reset();
	for (auto& slot : _slots)
	{
		slot.CompositeOwner.reset();
	}
}
bool ScenePostUI::NeedsResize() const
{
	return _remotePost ? _remotePost->Stack->NeedsResize() : _blur.NeedsTileResize();
}
void ScenePostUI::PrepareMaterials(Executor& executor, uint64_t frame, double animationSeconds, GDX12Texture* cloudImage)
{
	_posters->PrepareFrame(_resources, executor, frame, animationSeconds, cloudImage);
}
void ScenePostUI::Reset()
{
	_common.RequestHistoryReset = _common.ResetUpscalerHistory = true;
}
void ScenePostUI::Resize()
{
	if (_remotePost) _remotePost->Stack->Resize();
	else _reconstruction.QueryRenderTargetResolution();
	_pendingExports = {};
	_publishedInputs.clear();
	_pendingPublished.clear();
	_exports.clear();
	_exports.resize(_resources.FrameConstants.size());
	_resize = true;
	if (_remoteHud)
		for (auto& frame : _remoteHud->Frames)
			frame.Game.RasterOwner.reset();
	_slots.clear();
	_slots.resize(_resources.FrameConstants.size());
	// Rebind new scene inputs before resizing the existing persistent passes.
	Reset();
}
Id ScenePostUI::Packet(Graph& graph, std::vector<ID3D12Resource*>& bindings, const char* name,
                       std::vector<std::pair<GDX12Resource*, Mode>> resources,
                       const std::function<void(GDX12CommandList*)>& record, std::vector<Id> dependencies,
                       GDX12Texture* renderTarget, State textureState)
{
	std::vector<Access> accesses;
	for (auto [resource, mode] : resources)
	{
		auto* native = resource->D3DResource.Get();
		const auto id = NativeWork::ResourceFor(graph, bindings, native, name);
		for (const auto& image : _publishedInputs)
		{
			if (image.Get() == native)
			{
				graph.Resources[id].Initial = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
			}
		}
		const State state = renderTarget && native == renderTarget->GetResource()->D3DResource.Get()
		                        ? State(D3D12_RESOURCE_STATE_RENDER_TARGET)
		                    : native->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER
		                        ? State(D3D12_RESOURCE_STATE_GENERIC_READ)
		                        : textureState;
		const auto found = std::find_if(accesses.begin(), accesses.end(), [id](auto a) { return a.Resource == id; });
		if (found == accesses.end())
		{
			accesses.push_back({id, mode, state});
		}
		else if (mode != Mode::Read)
		{
			found->Usage = Mode::ReadWrite;
		}
	}
	return _work.AddOwned(graph, name, std::move(accesses), bindings, record,
	                      "Post/UI cannot submit private queue signals", std::move(dependencies));
}

Id ScenePostUI::AppendFrameGeneration(Graph& graph, std::vector<ID3D12Resource*>& bindings, GDX12Texture* depth,
                                      Id dependency)
{
	if (_swapchain->GetFrameGeneration().GetStatus().Effective == GDX12FrameGenerationProvider::Off)
	{
		return dependency;
	}
	const auto resource = [](GDX12Texture* texture) { return texture->GetResource(); };
	auto* completeMotion = _blur.GetOutput(1)->GetTexture();
	// Export exactly the states promised by the existing FG tagging API. No
	// terminal transition may invalidate eValidUntilPresent resources.
	const auto fgId = Packet(
	    graph, bindings, "frame-generation prepare/tag DIRECT SDK boundary",
	    {{resource(depth), Mode::Read}, {resource(completeMotion), Mode::Read}},
	    [this, depth, completeMotion](auto* cmd)
	    { _swapchain->GetFrameGeneration().Prepare(cmd, depth, completeMotion, _common); }, {dependency}, nullptr,
	    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	auto& exports = _pendingExports;
	const std::array<GDX12Texture*, 2> retained{depth, completeMotion};
	for (size_t index = 0; index < retained.size(); ++index)
	{
		auto resource = retained[index]->GetResource()->D3DResource;
		const auto id = NativeWork::ResourceFor(graph, bindings, resource.Get(), "FG retained input");
		graph.Resources[id].Final = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		if (std::none_of(_pendingPublished.begin(), _pendingPublished.end(),
		                 [&](const auto& previous) { return previous.Get() == resource.Get(); }))
		{
			_pendingPublished.push_back(resource);
		}
		exports[index] = std::move(resource);
	}
	return fgId;
}
Id ScenePostUI::AppendReconstruction(Graph& graph, std::vector<ID3D12Resource*>& bindings, GDX12Texture* hdr,
                                     GDX12Texture* depth, GDX12Texture* motion, GDX12Texture* featureMotion,
                                     GDX12Texture* target, std::vector<Id> dependencies)
{
	const auto resource = [](GDX12Texture* texture) { return texture->GetResource(); };
	_blur.PrepareRecording((_reconstruction.GetFlags() & RENDER_PASS_FLAG_USE_JITTER) ||
	                       GetFrameGenerationProvider() != GDX12FrameGenerationProvider::Off);
	auto* color = _blur.GetOutput(0)->GetTexture();
	auto* completeMotion = _blur.GetOutput(1)->GetTexture();
	_reconstruction.SetInputs({depth, completeMotion, color});
	if (_reconstruction._needsInitialize)
	{
		_reconstruction.Initialize();
	}
	else if (_resize)
	{
		_reconstruction.Resize();
	}
	_reconstruction.PrepareRecording();
	_reconstruction.LinkOutput();
	_resize = false;
	_output.SetInputs({_reconstruction.GetOutput(0), target});
	if (!_outputInitialized)
	{
		_output.Initialize();
		_outputInitialized = true;
	}

	if (_blur._prepareMotion)
	{
		auto& camera = _resources.FrameConstants[_resources.CurrFrameConstantsIndex]->CameraCB->GetResource();
		std::vector<std::pair<GDX12Resource*, Mode>> accesses{{resource(hdr), Mode::Read},
		                                                      {resource(depth), Mode::Read},
		                                                      {resource(motion), Mode::Read},
		                                                      {&camera, Mode::Read},
		                                                      {resource(_blur._motion.get()), Mode::ReadWrite},
		                                                      {resource(_blur._velocity.get()), Mode::ReadWrite}};
		if (_blur._preparedBlur)
		{
			for (auto* image : {_blur._color.get(), _blur._tileMax.get(), _blur._neighborMax.get()})
			{
				accesses.emplace_back(resource(image), Mode::ReadWrite);
			}
		}
		if (featureMotion)
		{
			accesses.emplace_back(resource(featureMotion), Mode::Read);
		}
		const auto blur = Packet(
		    graph, bindings, "motion preparation / blur DIRECT boundary", std::move(accesses),
		    [this, featureMotion](auto* cmd) { _blur.RecordCommands(cmd, featureMotion); }, std::move(dependencies));
		dependencies = {blur};
	}
	auto* reconstructed = _reconstruction.GetOutput(0)->GetTexture();
	const bool native = _reconstruction.GetStatus().Effective == GDX12UpscalerProvider::Native;
	if (!native)
	{
		auto* taa = _reconstruction.TAA();
		if (taa)
		{
			const auto frame = taa->PreparedFrame();
			std::vector<std::pair<GDX12Resource*, Mode>> accesses;
			for (auto* input : frame.Reads)
			{
				accesses.emplace_back(resource(input), Mode::Read);
			}
			accesses.emplace_back(resource(frame.Color), Mode::Write);
			accesses.emplace_back(resource(frame.Depth), Mode::Write);
			accesses.emplace_back(frame.Camera, Mode::Read);
			const auto reconstruction = Packet(
			    graph, bindings, "TAA resolve DIRECT boundary", std::move(accesses),
			    [taa, frame](auto* cmd) { taa->RecordCommands(cmd, frame); }, std::move(dependencies));
			dependencies = {reconstruction};
		}
		else
		{
			std::vector<std::pair<GDX12Resource*, Mode>> accesses{{resource(color), Mode::Read},
			                                                      {resource(reconstructed), Mode::ReadWrite}};
			if (_reconstruction.GetFlags() & RENDER_PASS_FLAG_USE_JITTER)
			{
				accesses.emplace_back(resource(depth), Mode::Read);
				accesses.emplace_back(resource(completeMotion), Mode::Read);
			}
			const auto reconstruction = Packet(
			    graph, bindings, "reconstruction provider DIRECT SDK boundary", std::move(accesses),
			    [this](auto* cmd) { _reconstruction.Execute(cmd); }, std::move(dependencies));
			dependencies = {reconstruction};
		}
	}
	return Packet(
	    graph, bindings, "output HDR display transform",
	    {{resource(reconstructed), Mode::Read}, {resource(target), Mode::Write}},
	    [this, native](auto* cmd)
	    {
		    _output.Execute(cmd);
		    if (native)
		    {
			    _reconstruction.Execute(cmd);
		    }
	    },
	    std::move(dependencies), target);
}
ScenePostUI::CapturedScreen ScenePostUI::CaptureScreen(Graph& graph, std::vector<ID3D12Resource*>& bindings,
    ID3D12Resource* backBuffer, const SceneTextureInspector::Input* post)
{
	const auto hud = GameHUD::Enabled.GetValue() && ImGui::GetCurrentContext();
	GameHUD::Bar bar{};
	if (hud)
	{
		if (!_gameOverlay)
			_gameOverlay = std::make_unique<ImDrawList>(ImGui::GetDrawListSharedData());
		bar = GameHUD::Draw(*_gameOverlay, _common.GameTimer ? _common.GameTimer->TotalTime() : 0.f,
		                   BistroInfoPanel::Height(ImGui::GetIO().DisplaySize.x));
	}
	const auto previews = _inspector.Append({_inspector, graph, bindings, _work, *_resources.Device, backBuffer},
	    post, _resources.CurrFrameConstantsIndex, _common.ActiveCameraNearPlane, _common.ActiveCameraFarPlane);
	auto overlay = Engine::UI::CaptureScreenFrame();
	return {std::move(overlay), hud ? Engine::UI::CaptureScreenOverlay(*_gameOverlay) : nullptr, previews, bar};
}

void ScenePostUI::EnsureHudSource()
{
	if (_resources.TextureReplicaLoaders.find(HudAtlasName) != _resources.TextureReplicaLoaders.end()) return;
	const auto path = std::filesystem::path(ASSETS_FOLDER) / "Textures/UI/BistroDemoHUD.png";
	_render.SetTextureReplicaSource(HudAtlasName, Engine::Core::ETextureType::Color, [path](auto&) {
		return Engine::Core::AssetManager::GetInstance().LoadTexture(path);
	});
}

ScenePostUI::HudRaster& ScenePostUI::LocalHud(GDX12DeviceResources& atlasSources)
{
	if (&atlasSources != &_resources)
	{
		const auto& source = atlasSources.TextureReplicaLoaders.at(HudAtlasName);
		if (!_localHud || _localHud->AtlasRevision != source.Revision)
		{
			auto* atlas = source.Load(_resources);
			if (!_localHud) _localHud = std::make_unique<HudRaster>(_resources, *atlas);
			else _localHud->Atlas = atlas;
			_localHud->AtlasRevision = source.Revision;
		}
	}
	else if (!_localHud)
	{
		auto* texture = _render.GetTextureByName(HudAtlasName);
		if (!texture)
		{
			const auto* asset = Engine::Core::AssetManager::GetInstance().LoadTexture(
			    std::filesystem::path(ASSETS_FOLDER) / "Textures/UI/BistroDemoHUD.png");
			if (!asset) throw std::runtime_error("Missing Bistro demo HUD atlas");
			texture = _render.CreateTexture(HudAtlasName, asset, Engine::Core::ETextureType::Color);
		}
		if (!texture || !texture->PrimaryDeviceTexture) throw std::runtime_error("Cannot create Bistro demo HUD atlas");
		_localHud = std::make_unique<HudRaster>(_resources, *texture->PrimaryDeviceTexture);
	}
	return *_localHud;
}

void ScenePostUI::PrepareRemoteHud(Id owner, Id display, UINT width, UINT height, const CapturedScreen& captured)
{
	EnsureHudSource();
	const UINT slot = _resources.CurrFrameConstantsIndex;
	auto& screen = _slots.at(slot);
	if (!_remoteHud || _remoteHud->Owner != owner)
		_remoteHud = std::shared_ptr<RemoteHud>(new RemoteHud(_hudSubmissions, owner, UINT(_slots.size())), RemoteHud::Retire);
	screen.HudOwner = _remoteHud;
	auto& remote = *screen.HudOwner;
	if (!remote.GPU.TryBegin(slot)) remote.GPU.Begin(slot);
	auto& frame = remote.Frames.at(slot);
	frame.Game.RasterOwner.reset();
	remote.RefreshAtlas(_resources.TextureReplicaLoaders.at(HudAtlasName));
	frame.Game.Draw = captured.Game;
	frame.Plan = {};
	frame.Plan.Device = owner;
	frame.Plan.FrameBoundary = false;
	frame.Bindings.clear();
	frame.Work.Commands.clear();
	frame.RasterReady = frame.Ready = frame.Imported = frame.Applied = frame.Completed = {};
	frame.Number = _hudFrame;
	frame.Overlay = bool(captured.Overlay);
	remote.PrepareLayer(frame, display, width, height);
	frame.Raster = remote.Raster->Append(frame.Plan, frame.Bindings, frame.Work, frame.Game, *frame.Layer,
	    remote.Renderer, captured.Bar);
	frame.Transfer.Copy(frame.Plan, frame.Bindings, frame.Work, "owner export game HUD layer", *frame.Layer);
}

Id ScenePostUI::CompositeScreen(Graph& graph, std::vector<ID3D12Resource*>& bindings, GDX12Texture* target,
    GDX12Texture* layer, Id ready, const SceneTextureInspector::Atlases& previews,
    const std::shared_ptr<const Engine::UI::DrawData>& overlay,
    bool hasGame)
{
	if (!overlay && !hasGame) return ready;
	const auto resource = [](GDX12Texture* texture) { return texture->GetResource(); };
	auto& screen = _slots.at(_resources.CurrFrameConstantsIndex);
	if (!_composite) _composite = std::make_unique<Engine::UI::ScreenComposite>(*_resources.Device);
	if (overlay)
	{
		if (!_screen)
			_screen = std::make_unique<Engine::UI::OffscreenRenderer>(*_resources.Device, UINT(2 * _slots.size()), true);
		std::vector<std::pair<GDX12Resource*, Mode>> accesses{{resource(layer), hasGame ? Mode::ReadWrite : Mode::Write}};
		std::vector<Engine::UI::TextureBinding> textures;
		for (UINT i = 0; i < previews.size(); ++i)
		{
			auto* preview = previews[i];
			if (!preview) continue;
			auto* atlas = resource(preview);
			accesses.emplace_back(atlas, Mode::Read);
			textures.push_back({SceneTextureInspector::AtlasId(i), atlas->D3DResource, preview->GetFormat()});
		}
		ready = Packet(graph, bindings, "Information and debug UI raster", std::move(accesses),
		    [this, &screen, hasGame, layer, overlay, textures = std::move(textures)](auto* cmd)
		    {
			    screen.RasterOwner = _screen->TryRecord(*cmd, layer->GetResource()->D3DResource, overlay, textures, !hasGame);
			    if (!screen.RasterOwner) throw std::logic_error("Screen UI backend slot is still in flight");
		    }, {ready}, layer);
	}
	return Packet(graph, bindings, "Engine.UI screen composition",
	    {{resource(layer), Mode::Read}, {resource(target), Mode::ReadWrite}},
	    [this, &screen, layer, target](auto* cmd)
	    {
		    screen.CompositeOwner = _composite->Record(*cmd, layer->GetResource()->D3DResource, target->GetResource()->D3DResource);
	    }, {ready}, target);
}

void ScenePostUI::AppendScreen(Graph& graph, std::vector<ID3D12Resource*>& bindings, GDX12Texture* target,
    const std::function<void(GDX12CommandList*)>& finish, Id dependency, const CapturedScreen& captured)
{
	const auto resource = [](GDX12Texture* texture) { return texture->GetResource(); };
	const UINT slot = _resources.CurrFrameConstantsIndex;
	auto& screen = _slots.at(slot);
	screen.RasterOwner.reset();
	screen.Game.RasterOwner.reset();
	screen.CompositeOwner.reset();
	screen.HudImport = screen.HudComposite = Invalid;
	const auto bar = captured.Bar;
	screen.Draw = captured.Overlay;
	screen.Game.Draw = captured.Game;
	const Id owner = screen.Game.Draw ? _hudSubmissions->SelectAdapter(HudAdapter.GetValue()) : 0;
	if (!owner)
	{
		screen.HudOwner.reset();
		_remoteHud.reset();
	}
	Id screenId = dependency;
	if (screen.Draw || screen.Game.Draw)
	{
		if (!_screen && (screen.Draw || !owner))
			_screen = std::make_unique<Engine::UI::OffscreenRenderer>(*_resources.Device, UINT(2 * _slots.size()), true);
		if (!_composite)
			_composite = std::make_unique<Engine::UI::ScreenComposite>(*_resources.Device);
		const UINT width = target->GetWidth(), height = target->GetHeight();
		if (!screen.Target || screen.Target->GetWidth() != width || screen.Target->GetHeight() != height)
		{
			screen.Target = std::make_unique<GDX12Texture>();
			GDX12RenderHelpers::InitializeRenderTarget(*screen.Target, _resources, width, height,
			                                           Engine::UI::OffscreenFormat, {0, 0, 0, 0});
		}
		Id raster = dependency;
		if (owner)
		{
			PrepareRemoteHud(owner, 0, width, height, captured);
			auto& frame = screen.HudOwner->Frames.at(slot);
			raster = screen.HudImport = frame.Transfer.Copy(graph, bindings, _work, "GPU0 import game HUD layer", *screen.Target);
			graph.Operations[raster].Dependencies.push_back(dependency);
		}
		else if (screen.Game.Draw)
			raster = LocalHud(_resources).Append(graph, bindings, _work, screen.Game, *screen.Target, *_screen, bar, {dependency});
		screenId = CompositeScreen(graph, bindings, target, screen.Target.get(), raster, captured.Previews,
		    screen.Draw, bool(screen.Game.Draw));
		if (owner) screen.HudComposite = screenId;
	}
	Packet(graph, bindings, "frame end / PRESENT handoff", {{resource(target), Mode::ReadWrite}}, finish, {screenId},
	       nullptr, D3D12_RESOURCE_STATE_PRESENT);
}
Id ScenePostUI::AppendStack(Graph& graph, std::vector<ID3D12Resource*>& bindings, GDX12Texture* hdr, GDX12Texture* depth,
                         GDX12Texture* motion, GDX12Texture* featureMotion, GDX12Texture* target,
                         std::vector<Id> dependencies)
{
	if (!hdr || !depth || !motion || !target || hdr->GetWidth() != depth->GetWidth() ||
	    hdr->GetHeight() != depth->GetHeight() || hdr->GetWidth() != motion->GetWidth() ||
	    hdr->GetHeight() != motion->GetHeight())
	{
		throw std::invalid_argument("Ordinary post requires matching HDR/depth/motion grids");
	}
	_dof.SetInputs({hdr, depth});
	if (_dof.IsActive())
	{
		if (!_dof._output.IsInitialized())
		{
			_dof.Initialize();
		}
		else if (_resize || _dof._output.GetWidth() != hdr->GetWidth() || _dof._output.GetHeight() != hdr->GetHeight())
		{
			_dof.Resize();
		}
		_dof.PrepareRecording();
		auto* dof = _dof.GetOutput(0)->GetTexture();
		dependencies.push_back(Packet(
		    graph, bindings, "DoF SDK DIRECT recording boundary",
		    {{hdr->GetResource(), Mode::Read}, {depth->GetResource(), Mode::Read}, {dof->GetResource(), Mode::Write}},
		    [this](auto* cmd) { _dof.RecordCommands(cmd); }, dependencies));
		hdr = dof;
	}
	else
	{
		_dof.PrepareRecording(); // Notify temporal SR even when no DoF packet is needed.
	}
	_blur.SetInputs({hdr, depth, motion});
	if (!_initialized)
	{
		_blur.Initialize();
		_initialized = true;
	}
	else if (_resize)
	{
		_blur.Resize();
	}
	const auto output =
	    AppendReconstruction(graph, bindings, hdr, depth, motion, featureMotion, target, std::move(dependencies));
	return AppendFrameGeneration(graph, bindings, depth, output);
}
void ScenePostUI::Append(Graph& graph, std::vector<ID3D12Resource*>& bindings, GDX12Texture* hdr,
    GDX12Texture* depth, GDX12Texture* motion, GDX12Texture* featureMotion, GDX12Texture* target,
    const std::function<void(GDX12CommandList*)>& finish)
{
	if (_remotePost)
		AppendRemotePost(graph, bindings, hdr, depth, motion, featureMotion, target, finish);
	else
	{
		const auto output = AppendStack(graph, bindings, hdr, depth, motion, featureMotion, target);
		AppendScreen(graph, bindings, target, finish, output,
		    CaptureScreen(graph, bindings, target->GetResource()->D3DResource.Get()));
	}
}

void ScenePostUI::AppendRemotePost(Graph& scene, std::vector<ID3D12Resource*>& sceneBindings, GDX12Texture* hdr,
    GDX12Texture* depth, GDX12Texture* motion, GDX12Texture* featureMotion, GDX12Texture* target,
    const std::function<void(GDX12CommandList*)>& finish)
{
	auto& owner = *_remotePost;
	auto& stack = *owner.Stack;
	const UINT slot = _resources.CurrFrameConstantsIndex;
	if (!owner.GPU.TryBegin(slot)) owner.GPU.Begin(slot);
	_inspector.RetireReadback(owner.Owner, slot);
	owner.Resources.CurrFrameConstantsIndex = slot;
	const UINT cameraIndex = _common.ActiveCameraCBufferIndex;
	auto& camera = *owner.Resources.FrameConstants.at(slot)->CameraCB;
	if (camera.GetElementCount() <= cameraIndex) camera.Resize(cameraIndex + 1);
	camera.CopyData(cameraIndex, _resources.FrameConstants.at(slot)->CameraCB->GetCPUData()[cameraIndex]);
	stack.BeginPlan(owner.GPU, _hudFrame);

	auto& frame = owner.Frames.at(slot);
	frame.Plan = {};
	frame.Plan.Device = owner.Owner;
	frame.Bindings.clear();
	frame.Export = frame.Import = frame.SceneImport = frame.HudImport = frame.InspectorImport = Invalid;
	frame.PeerHud = frame.Presented = false;
	frame.Number = _hudFrame;
	frame.SceneReady = frame.Produced = {};
	frame.Bytes = {};
	frame.HudBytes = frame.UiBytes = 0;
	frame.HudOwner = Invalid;
	const auto native = [](GDX12Texture* texture) { return texture->GetResource()->D3DResource.Get(); };
	const std::array sources{hdr, depth, motion, featureMotion};
	std::unordered_set<ID3D12Resource*> live;
	for (auto* source : sources) if (source) live.insert(native(source));
	frame.Inputs->Begin();
	frame.Inputs->RetireUnused(live, [](auto*) {});
	std::array<GDX12Texture*, 4> inputs{};
	for (UINT i = 0; i < sources.size(); ++i)
	{
		auto* source = sources[i];
		if (!source) continue;
		const auto id = NativeWork::ResourceFor(scene, sceneBindings, native(source), "Bistro post scene input");
		auto* copy = frame.Inputs->Mirror(scene, sceneBindings, native(source), id, true);
		frame.Bytes[i] = frame.Inputs->PendingTransferBytes(native(source));
		auto& view = frame.Views[i];
		if (!view.IsInitialized() || native(&view) != copy)
		{
			GDX12TextureDesc desc;
			desc.Width = source->GetWidth();
			desc.Height = source->GetHeight();
			desc.Format = source->GetFormat();
			desc.SRVDesc = source->ShaderViewDescription();
			desc.SRV_UAV_Heap = owner.Resources.SRV_UAV_Heap.get();
			desc.SRVHeapIndex = view.IsInitialized() ? view.GetSRV()->HeapIndex
			    : desc.SRV_UAV_Heap->GetAvailableIndex(SceneDescriptorStart);
			desc.ExternalResource = ComPtr<ID3D12Resource>(copy);
			view.Initialize(desc);
		}
		inputs[i] = &view;
	}
	frame.BundleBytes = frame.Inputs->TransferBytes();
	frame.Export = frame.Inputs->AppendExport(scene, sceneBindings, _work, Queue::Direct);
	scene.Operations[frame.Export].Name = "GPU0 export current-frame Bistro post inputs";
	frame.SceneImport = frame.Inputs->AppendImport(frame.Plan, frame.Bindings, stack._work);
	frame.Plan.Operations[frame.SceneImport].Name = "owner import current-frame Bistro post inputs";

	const UINT width = target->GetWidth(), height = target->GetHeight();
	auto* output = target;
	if (!owner.Direct)
	{
		EnsureScreenTexture(frame.Output, owner.Resources, width, height);
		output = frame.Output.get();
	}
	const Id post = stack.AppendStack(frame.Plan, frame.Bindings, inputs[0], inputs[1], inputs[2], inputs[3], output,
	    {frame.SceneImport});
	const SceneTextureInspector::Input postTextures{stack._inspector, frame.Plan, frame.Bindings, stack._work,
	    *owner.Resources.Device, native(output)};
	_inspector.AppendReadback(postTextures, slot, false);
	auto captured = CaptureScreen(scene, sceneBindings, native(target), &postTextures);
	const UINT exportedAtlas = owner.Direct ? 0 : 1;
	Id previewExport = Invalid;
	if (auto* preview = captured.Previews[exportedAtlas])
	{
		auto& displayResources = owner.Direct ? owner.Resources : _resources;
		auto& exportGraph = owner.Direct ? scene : frame.Plan;
		auto& exportBindings = owner.Direct ? sceneBindings : frame.Bindings;
		auto& exportWork = owner.Direct ? _work : stack._work;
		EnsureScreenTexture(frame.Preview, displayResources, preview->GetWidth(), preview->GetHeight());
		frame.Inspector.Prepare(*_hudSubmissions, exportGraph.Device, owner.Direct ? owner.Owner : 0, native(preview)->GetDesc());
		previewExport = frame.Inspector.Copy(exportGraph, exportBindings, exportWork, "export texture inspector atlas", *preview);
		captured.Previews[exportedAtlas] = frame.Preview.get();
		frame.UiBytes = frame.Inspector.Bytes;
	}

	if (!owner.Direct)
	{
		frame.Display.Prepare(*_hudSubmissions, owner.Owner, 0, native(output)->GetDesc());
		frame.Display.Copy(frame.Plan, frame.Bindings, stack._work, "owner export display color", *output);
		frame.Import = frame.Display.Copy(scene, sceneBindings, _work, "GPU0 import display color", *target);
		scene.Operations[frame.Import].Dependencies.push_back(frame.Export);
		if (previewExport != Invalid)
		{
			const Id previewImport = frame.Inspector.Copy(scene, sceneBindings, _work,
			    "GPU0 import texture inspector atlas", *frame.Preview);
			// The display import waits for the owner terminal fence, which also covers the atlas export.
			scene.Operations[previewImport].Dependencies.push_back(frame.Import);
		}
		AppendScreen(scene, sceneBindings, target, finish, frame.Import, captured);
		if (_slots.at(slot).Game.Draw)
		{
			const auto& hud = _slots.at(slot).HudOwner;
			frame.HudOwner = hud ? hud->Owner : 0;
			if (hud) frame.HudBytes = hud->Frames.at(slot).Transfer.Bytes;
		}
		return;
	}

	auto& sceneSlot = _slots.at(slot);
	sceneSlot.Game.RasterOwner.reset();
	sceneSlot.Game.Draw = captured.Game;
	sceneSlot.HudImport = sceneSlot.HudComposite = Invalid;
	const Id hudOwner = captured.Game ? _hudSubmissions->SelectAdapter(HudAdapter.GetValue()) : Invalid;
	frame.HudOwner = hudOwner;
	if (hudOwner == Invalid || hudOwner == owner.Owner || hudOwner == 0)
	{
		sceneSlot.HudOwner.reset();
		_remoteHud.reset();
	}
	auto& screen = stack._slots.at(slot);
	screen.Game.RasterOwner.reset();
	screen.Game.Draw = captured.Game;
	screen.RasterOwner.reset();
	screen.CompositeOwner.reset();

	Id ready = post;
	std::vector<Id> exports{frame.Export};
	if (previewExport != Invalid)
	{
		exports.push_back(previewExport);
		frame.InspectorImport = frame.Inspector.Copy(frame.Plan, frame.Bindings, stack._work,
		    "display import texture inspector atlas", *frame.Preview);
	}
	if (captured.Game || captured.Overlay)
	{
		EnsureScreenTexture(screen.Target, owner.Resources, width, height);
		if (hudOwner == owner.Owner)
		{
			EnsureHudSource();
			if (!stack._screen)
				stack._screen = std::make_unique<Engine::UI::OffscreenRenderer>(*owner.Resources.Device, UINT(2 * _slots.size()), true);
			ready = stack.LocalHud(_resources).Append(frame.Plan, frame.Bindings, stack._work, screen.Game,
			    *screen.Target, *stack._screen, captured.Bar, {post});
		}
		else if (hudOwner == 0)
		{
			EnsureScreenTexture(sceneSlot.Target, _resources, width, height);
			if (!_screen)
				_screen = std::make_unique<Engine::UI::OffscreenRenderer>(*_resources.Device, UINT(2 * _slots.size()), true);
			LocalHud(_resources).Append(scene, sceneBindings, _work, sceneSlot.Game, *sceneSlot.Target, *_screen, captured.Bar);
			frame.Hud.Prepare(*_hudSubmissions, 0, owner.Owner, native(sceneSlot.Target.get())->GetDesc());
			exports.push_back(frame.Hud.Copy(scene, sceneBindings, _work, "GPU0 export pure game HUD", *sceneSlot.Target));
			frame.HudImport = frame.Hud.Copy(frame.Plan, frame.Bindings, stack._work, "display import game HUD layer", *screen.Target);
			frame.HudBytes = frame.Hud.Bytes;
		}
		else if (hudOwner != Invalid)
		{
			PrepareRemoteHud(hudOwner, owner.Owner, width, height, captured);
			auto& hud = sceneSlot.HudOwner->Frames.at(slot);
			frame.PeerHud = true;
			frame.HudImport = hud.Transfer.Copy(frame.Plan, frame.Bindings, stack._work, "display import peer game HUD layer", *screen.Target);
			frame.HudBytes = hud.Transfer.Bytes;
		}
		ready = stack.CompositeScreen(frame.Plan, frame.Bindings, target, screen.Target.get(), ready, captured.Previews,
		    captured.Overlay, bool(captured.Game));
	}
	stack.Packet(frame.Plan, frame.Bindings, "Bistro owner PRESENT handoff",
	    {{target->GetResource(), Mode::ReadWrite}}, [](auto*) {}, {ready}, nullptr, D3D12_RESOURCE_STATE_PRESENT);
	// The scene fence remains on GPU0 and covers its timer and every scene-side export.
	frame.Export = Packet(scene, sceneBindings, "GPU0 scene and post inputs complete", {}, finish, std::move(exports));
	scene.Operations[frame.Export].SideEffect = true;
}

void ScenePostUI::AddPostExchange(const Schedule& primary, std::vector<Executor::PacketExchange>& exchanges)
{
	if (!_remotePost) return;
	auto& owner = *_remotePost;
	const UINT slot = _resources.CurrFrameConstantsIndex;
	auto& frame = owner.Frames.at(slot);
	auto schedule = Compile(frame.Plan);
	owner.GPU.Reserve(schedule);
	const Id sceneImport = HudPacket(schedule, frame.SceneImport);
	const Id hudImport = frame.HudImport == Invalid ? Invalid : HudPacket(schedule, frame.HudImport);
	const Id inspectorImport = frame.InspectorImport == Invalid ? Invalid : HudPacket(schedule, frame.InspectorImport);
	exchanges.push_back({HudPacket(primary, frame.Export), frame.Import == Invalid ? Invalid : HudPacket(primary, frame.Import),
	    [this, &owner, &frame, schedule = std::move(schedule), slot, sceneImport, hudImport, inspectorImport](CompletionToken ready)
	    {
		    auto& stack = *owner.Stack;
		    std::vector<PacketWait> waits{{sceneImport, ready}};
		    if (hudImport != Invalid)
			    waits.push_back({hudImport, frame.PeerHud ? _slots.at(slot).HudOwner->Frames.at(slot).Ready : ready});
		    if (inspectorImport != Invalid) waits.push_back({inspectorImport, ready});
		    owner.GPU.Execute(schedule, frame.Bindings, [&stack](Id operation, UINT, auto* cmd) {
			    if (!stack.Record(operation, cmd)) throw std::logic_error("Missing Bistro owner post work");
		    }, nullptr, nullptr, nullptr, waits);
		    _inspector.ReadbackSubmitted(owner.Owner, slot);
		    stack.Submitted(slot);
		    frame.SceneReady = ready;
		    return frame.Produced = owner.GPU.Completions().back();
	    }});
}

void ScenePostUI::PostPresented()
{
	if (!_remotePost || _remotePost->Reported) return;
	auto& owner = *_remotePost;
	owner.Frames.at(_resources.CurrFrameConstantsIndex).Presented = true;
	const auto& submit = *owner.GPU.Submissions();
	const auto presented = std::find_if(owner.Frames.begin(), owner.Frames.end(), [&](const auto& frame) {
		return frame.Presented && frame.Produced && submit.Complete(frame.Produced);
	});
	if (presented == owner.Frames.end()) return;
	auto& frame = *presented;
	owner.Reported = true;
	const auto& status = owner.Stack->_reconstruction.GetStatus();
	Log::Info("RenderGraph", "Bistro post Present S_OK frame=", frame.Number,
	    " scene=", submit.AdapterLabel(0), " post=", submit.AdapterLabel(owner.Owner),
	    " HUD=", frame.HudOwner == Invalid ? "disabled" : submit.AdapterLabel(frame.HudOwner),
	    " swapchain=", submit.AdapterLabel(owner.Direct ? owner.Owner : 0),
	    " provider=", int(status.Effective), " quality=", int(status.Requested.Quality),
	    " FG=", int(_swapchain->GetFrameGeneration().GetStatus().Effective),
	    " render=", frame.Views[0].GetWidth(), "x", frame.Views[0].GetHeight(),
	    " display=", _common.WindowWidth, "x", _common.WindowHeight,
	    " hdrBytes=", frame.Bytes[0], " depthBytes=", frame.Bytes[1], " motionBytes=", frame.Bytes[2],
	    " featureMotionBytes=", frame.Bytes[3], " sceneBundleBytes=", frame.BundleBytes,
	    " hudBytes=", frame.HudBytes, " inspectorBytes=", frame.UiBytes,
	    " displayColorReturnBytes=", owner.Direct ? 0 : frame.Display.Bytes,
	    " sceneFence=", frame.SceneReady.Value, " postFence=", frame.Produced.Value);
}

std::vector<CanvasRead> ScenePostUI::AppendPosters(Graph& graph, std::vector<ID3D12Resource*>& bindings)
{
	for (Id id = 0; id < bindings.size(); ++id)
	{
		for (const auto& image : _publishedInputs)
		{
			if (image.Get() == bindings[id])
			{
				graph.Resources[id].Initial = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
				// imported SDK state valid until the displayed plan uses it.
				graph.Resources[id].Final = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
			}
		}
	}
	return _posters->Append(graph, bindings, _work);
}
} // namespace BistroRender

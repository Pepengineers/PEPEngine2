#include "ScenePostUI.h"
#include "BistroInfoPanel.h"
#include "BistroGameHUD.h"
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
AutoConsoleVariable<int> HudAdapter(L"App.UI.Game.Adapter", -1,
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
	std::unique_ptr<GDX12RootSignature> Root;
	ComPtr<ID3D12PipelineState> Pipeline;
};

struct ScenePostUI::RemoteHud
{
	struct Frame
	{
		HudFrame Game;
		std::unique_ptr<GDX12Texture> Layer;
		QueueSubmission::TransferBuffer Transfer;
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT Footprint{};
		Graph Plan;
		NativeWork Work;
		std::vector<ID3D12Resource*> Bindings;
		Id Raster = Invalid;
		CompletionToken RasterReady, Ready, Imported, Applied, Completed;
		UINT64 Number = 0, Bytes = 0;
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

	void PrepareLayer(Frame& frame, UINT width, UINT height)
	{
		if (frame.Layer && frame.Layer->GetWidth() == width && frame.Layer->GetHeight() == height) return;
		frame.Layer = std::make_unique<GDX12Texture>();
		GDX12RenderHelpers::InitializeRenderTarget(*frame.Layer, Resources, width, height,
		    Engine::UI::OffscreenFormat, {0, 0, 0, 0});
		const auto desc = frame.Layer->GetResource()->D3DResource->GetDesc();
		const auto& submit = *GPU.Submissions();
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT display{};
		UINT64 displayBytes = 0;
		Resources.Device->GetDevice()->GetCopyableFootprints(&desc, 0, 1, 0, &frame.Footprint, nullptr, nullptr, &frame.Bytes);
		submit.Device(0).GetDevice()->GetCopyableFootprints(&desc, 0, 1, 0, &display, nullptr, nullptr, &displayBytes);
		const auto& footprint = frame.Footprint;
		if (!frame.Bytes || frame.Bytes == UINT64_MAX || displayBytes != frame.Bytes || display.Offset != footprint.Offset ||
		    display.Footprint.RowPitch != footprint.Footprint.RowPitch || display.Footprint.Width != footprint.Footprint.Width ||
		    display.Footprint.Height != footprint.Footprint.Height || display.Footprint.Depth != footprint.Footprint.Depth ||
		    display.Footprint.Format != footprint.Footprint.Format)
			throw std::runtime_error("Game HUD requires matching adapter texture footprints");
		frame.Transfer = submit.CreateTransferBuffer(Owner, frame.Bytes);
	}

	Id Copy(Frame& frame, Graph& graph, std::vector<ID3D12Resource*>& bindings, NativeWork& work,
	        GDX12Texture& texture, bool exporting)
	{
		const CD3DX12_TEXTURE_COPY_LOCATION image(texture.GetResource()->D3DResource.Get(), 0u);
		const CD3DX12_TEXTURE_COPY_LOCATION buffer(frame.Transfer.Resources[exporting ? 1 : 0].Get(), frame.Footprint);
		const auto& source = exporting ? image : buffer;
		const auto& dest = exporting ? buffer : image;
		const Id read = NativeWork::ResourceFor(graph, bindings, source.pResource, "game HUD copy source");
		const Id write = NativeWork::ResourceFor(graph, bindings, dest.pResource, "game HUD copy destination");
		return work.Add(graph, exporting ? "owner export game HUD layer" : "GPU0 import game HUD layer",
		    {{read, Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE}, {write, Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST}},
		    [source, dest](auto* cmd) { cmd->CopyTextureRegion(&dest, 0, 0, 0, &source, nullptr); });
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
		    " format=RGBA8_UNORM premultiplied=1 sceneInputBytes=0 resultBytes=", frame.Bytes,
		    " rowPitch=", frame.Footprint.Footprint.RowPitch, " ownerRasterFence=", frame.RasterReady.Value,
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
	screen.HudImport = HudPacket(primary, screen.HudImport);
	screen.HudComposite = HudPacket(primary, screen.HudComposite);
	owner.GPU.Reserve(schedule);
	owner.GPU.Execute(schedule, frame.Bindings, [&](Id op, UINT, auto* cmd) {
		if (!frame.Work.Record(op, cmd)) throw std::logic_error("Missing owner game HUD work");
	});
	frame.RasterReady = owner.GPU.Completions().at(raster);
	frame.Ready = owner.GPU.Completions().back();
	waits.push_back({screen.HudImport, frame.Ready});
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
	if (!screen.HudOwner || completions.empty()) return;
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

ScenePostUI::ScenePostUI(RenderPipelineContext& c)
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
	_posters = std::make_unique<BistroUIPosters>(c.Runtime,
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
	_gameOverlay.reset();
	for (auto& slot : _slots)
	{
		slot.CompositeOwner.reset();
	}
}
bool ScenePostUI::NeedsResize() const
{
	return _blur.NeedsTileResize();
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
	_reconstruction.QueryRenderTargetResolution();
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
void ScenePostUI::AppendScreen(Graph& graph, std::vector<ID3D12Resource*>& bindings, GDX12Texture* target,
                               const std::function<void(GDX12CommandList*)>& finish, Id dependency)
{
	const auto resource = [](GDX12Texture* texture) { return texture->GetResource(); };
	const UINT slot = _resources.CurrFrameConstantsIndex;
	auto& screen = _slots.at(slot);
	screen.RasterOwner.reset();
	screen.Game.RasterOwner.reset();
	screen.CompositeOwner.reset();
	screen.HudImport = screen.HudComposite = Invalid;
	const auto hud = GameHUD::Enabled.GetValue() && ImGui::GetCurrentContext();
	GameHUD::Bar bar{};
	if (hud)
	{
		if (!_gameOverlay)
			_gameOverlay = std::make_unique<ImDrawList>(ImGui::GetDrawListSharedData());
		bar = GameHUD::Draw(*_gameOverlay, _common.GameTimer ? _common.GameTimer->TotalTime() : 0.f,
		                   BistroInfoPanel::Height(ImGui::GetIO().DisplaySize.x));
	}
	auto* preview = _inspector.Append(graph, bindings, _work, *_resources.Device, slot,
	    resource(target)->D3DResource.Get(), _common.ActiveCameraNearPlane, _common.ActiveCameraFarPlane);
	screen.Draw = Engine::UI::CaptureScreenFrame();
	screen.Game.Draw = hud ? Engine::UI::CaptureScreenOverlay(*_gameOverlay) : nullptr;
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
			if (!_remoteHud || _remoteHud->Owner != owner)
			{
				const auto path = std::filesystem::path(ASSETS_FOLDER) / "Textures/UI/BistroDemoHUD.png";
				if (_resources.TextureReplicaLoaders.find(HudAtlasName) == _resources.TextureReplicaLoaders.end())
					_render.SetTextureReplicaSource(HudAtlasName, Engine::Core::ETextureType::Color, [path](auto&) {
						return Engine::Core::AssetManager::GetInstance().LoadTexture(path);
					});
				_remoteHud = std::shared_ptr<RemoteHud>(
				    new RemoteHud(_hudSubmissions, owner, UINT(_slots.size())), RemoteHud::Retire);
			}
			screen.HudOwner = _remoteHud;
			auto& remote = *screen.HudOwner;
			// A successful primary frame has already joined this work at the HUD import.
			if (!remote.GPU.TryBegin(slot)) remote.GPU.Begin(slot);
			auto& frame = remote.Frames.at(slot);
			frame.Game.RasterOwner.reset();
			remote.RefreshAtlas(_resources.TextureReplicaLoaders.at(HudAtlasName));
			frame.Game.Draw = screen.Game.Draw;
			frame.Plan = {};
			frame.Plan.Device = owner;
			frame.Plan.FrameBoundary = false;
			frame.Bindings.clear();
			frame.Work.Commands.clear();
			frame.RasterReady = frame.Ready = frame.Imported = frame.Applied = frame.Completed = {};
			frame.Number = _hudFrame;
			frame.Overlay = bool(screen.Draw);
			remote.PrepareLayer(frame, width, height);
			frame.Raster = remote.Raster->Append(frame.Plan, frame.Bindings, frame.Work, frame.Game, *frame.Layer,
			    remote.Renderer, bar);
			remote.Copy(frame, frame.Plan, frame.Bindings, frame.Work, *frame.Layer, true);
			raster = screen.HudImport = remote.Copy(frame, graph, bindings, _work, *screen.Target, false);
			graph.Operations[raster].Dependencies.push_back(dependency);
		}
		else if (screen.Game.Draw)
		{
			if (!_localHud)
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
			raster = _localHud->Append(graph, bindings, _work, screen.Game, *screen.Target, *_screen, bar, {dependency});
		}
		if (screen.Draw)
		{
			const bool hasGame = bool(screen.Game.Draw);
			std::vector<std::pair<GDX12Resource*, Mode>> overlayAccesses{
			    {resource(screen.Target.get()), hasGame ? Mode::ReadWrite : Mode::Write}};
			std::vector<Engine::UI::TextureBinding> overlayTextures;
			if (preview)
			{
				auto* atlas = resource(preview);
				overlayAccesses.emplace_back(atlas, Mode::Read);
				overlayTextures.push_back({SceneTextureInspector::AtlasId, atlas->D3DResource, preview->GetFormat()});
			}
			raster = Packet(graph, bindings, "Information and debug UI raster", std::move(overlayAccesses),
			    [this, &screen, hasGame, overlayTextures = std::move(overlayTextures)](auto* cmd)
			    {
				    screen.RasterOwner = _screen->TryRecord(*cmd, screen.Target->GetResource()->D3DResource,
				        screen.Draw, overlayTextures, !hasGame);
				    if (!screen.RasterOwner) throw std::logic_error("Screen UI backend slot is still in flight");
			    }, {raster}, screen.Target.get());
		}
		screenId = Packet(
		    graph, bindings, "Engine.UI screen composition",
		    {{resource(screen.Target.get()), Mode::Read}, {resource(target), Mode::ReadWrite}},
		    [this, &screen, target](auto* cmd)
		    {
			    screen.CompositeOwner = _composite->Record(*cmd, screen.Target->GetResource()->D3DResource,
			                                               target->GetResource()->D3DResource);
		    },
		    {raster}, target);
		if (owner) screen.HudComposite = screenId;
	}
	Packet(graph, bindings, "frame end / PRESENT handoff", {{resource(target), Mode::ReadWrite}}, finish, {screenId},
	       nullptr, D3D12_RESOURCE_STATE_PRESENT);
}
void ScenePostUI::Append(Graph& graph, std::vector<ID3D12Resource*>& bindings, GDX12Texture* hdr, GDX12Texture* depth,
                         GDX12Texture* motion, GDX12Texture* featureMotion, GDX12Texture* target,
                         const std::function<void(GDX12CommandList*)>& finish)
{
	if (!hdr || !depth || !motion || !target || hdr->GetWidth() != depth->GetWidth() ||
	    hdr->GetHeight() != depth->GetHeight() || hdr->GetWidth() != motion->GetWidth() ||
	    hdr->GetHeight() != motion->GetHeight())
	{
		throw std::invalid_argument("Ordinary post requires matching HDR/depth/motion grids");
	}
	_dof.SetInputs({hdr, depth});
	std::vector<Id> dependencies;
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
		    [this](auto* cmd) { _dof.RecordCommands(cmd); }));
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
	const auto fg = AppendFrameGeneration(graph, bindings, depth, output);
	AppendScreen(graph, bindings, target, finish, fg);
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

#include "ScenePostUI.h"
#include "BistroInfoPanel.h"
#include "BistroGameHUD.h"
#include "App.Base/Modules/RenderModule.h"
#include "App.Base/Modules/SceneManagerModule.h"
#include "Engine.Core/BenchmarkEngine.h"
#include "Engine.Core/AssetManager.h"
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

namespace BistroRender
{
namespace
{
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
void ScenePostUI::PrepareMaterials(double animationSeconds, GDX12Texture* cloudImage)
{
	_posters->SetImageInput(cloudImage);
	_posters->PrepareFrame(_resources, _resources.CurrFrameConstantsIndex, animationSeconds);
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
	auto& screen = _slots.at(_resources.CurrFrameConstantsIndex);
	screen.RasterOwner.reset();
	screen.GameRasterOwner.reset();
	screen.CompositeOwner.reset();
	const bool hud = GameHUD::Enabled.GetValue() && ImGui::GetCurrentContext();
	GameHUD::Bar bar{};
	if (hud && !_hudAtlas)
	{
		auto* texture = _render.GetTextureByName("BistroDemoHUD");
		if (!texture)
		{
			const auto* asset = Engine::Core::AssetManager::GetInstance().LoadTexture(
			    std::filesystem::path(ASSETS_FOLDER) / "Textures/UI/BistroDemoHUD.png");
			if (!asset)
				throw std::runtime_error("Missing Bistro demo HUD atlas");
			texture = _render.CreateTexture("BistroDemoHUD", asset, Engine::Core::ETextureType::Color);
		}
		if (!texture || !texture->PrimaryDeviceTexture)
			throw std::runtime_error("Cannot create Bistro demo HUD atlas");
		_hudAtlas = texture->PrimaryDeviceTexture;
	}
	if (hud)
	{
		if (!_gameOverlay)
			_gameOverlay = std::make_unique<ImDrawList>(ImGui::GetDrawListSharedData());
		bar = GameHUD::Draw(*_gameOverlay, _common.GameTimer ? _common.GameTimer->TotalTime() : 0.f,
		                   BistroInfoPanel::Height(ImGui::GetIO().DisplaySize.x));
	}
	auto* preview = _inspector.Append(graph, bindings, _work, *_resources.Device, _resources.CurrFrameConstantsIndex,
	    resource(target)->D3DResource.Get(), _common.ActiveCameraNearPlane, _common.ActiveCameraFarPlane);
	screen.Draw = Engine::UI::CaptureScreenFrame();
	screen.GameDraw = hud ? Engine::UI::CaptureScreenOverlay(*_gameOverlay) : nullptr;
	Id screenId = dependency;
	if (screen.Draw || screen.GameDraw)
	{
		if (!_screen)
		{
			_screen = std::make_unique<Engine::UI::OffscreenRenderer>(*_resources.Device, UINT(2 * _slots.size()), true);
			_composite = std::make_unique<Engine::UI::ScreenComposite>(*_resources.Device);
		}
		if (!screen.Target)
		{
			screen.Target = std::make_unique<GDX12Texture>();
			GDX12RenderHelpers::InitializeRenderTarget(*screen.Target, _resources, _common.WindowWidth,
			                                           _common.WindowHeight, Engine::UI::OffscreenFormat, {0, 0, 0, 0});
		}
		std::vector<std::pair<GDX12Resource*, Mode>> accesses{{resource(screen.Target.get()), Mode::Write}};
		std::vector<Engine::UI::TextureBinding> hudTextures;
		std::vector<Id> rasterDependencies{dependency};
		if (screen.GameDraw)
		{
			auto* atlas = resource(_hudAtlas);
			NativeWork::ImportReadOnlyTexture(graph, bindings, *atlas, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
			                                  "game HUD atlas");
			accesses.emplace_back(atlas, Mode::Read);
			hudTextures.push_back({GameHUD::Atlas, atlas->D3DResource, _hudAtlas->GetFormat()});
		}
		if (bar.Sparks)
		{
			if (!_healthBarPipeline)
			{
				GDX12RootSignatureDesc root;
				root.Constants = {5};
				_healthBarRoot = std::make_unique<GDX12RootSignature>(_resources.Device, root);
				_healthBarPipeline = CreateHealthBarPipeline(*_resources.Device, *_healthBarRoot);
			}
			const UINT width = UINT(std::ceil(bar.Width)), height = UINT(std::ceil(bar.Height));
			if (!screen.HealthBar || screen.HealthBar->GetWidth() != width || screen.HealthBar->GetHeight() != height)
			{
				screen.HealthBar = std::make_unique<GDX12Texture>();
				GDX12RenderHelpers::InitializeRenderTarget(*screen.HealthBar, _resources, width, height,
				                                           Engine::UI::OffscreenFormat, {0, 0, 0, 0});
			}
			auto* barTarget = screen.HealthBar.get();
			auto* barResource = resource(barTarget);
			rasterDependencies.push_back(Packet(
			    graph, bindings, "Game HUD health bar (GPU sparks)", {{barResource, Mode::Write}},
			    [this, bar, barTarget](auto* commands)
			    {
				    auto* cmd = commands->GetCommandList().Get();
				    const auto rtv = barTarget->GetRTV()->CPUHandle;
				    const auto viewport = barTarget->GetViewport();
				    const auto scissor = barTarget->GetScissorRect();
				    cmd->SetGraphicsRootSignature(_healthBarRoot->GetRootSignature().Get());
				    cmd->SetPipelineState(_healthBarPipeline.Get());
				    cmd->SetGraphicsRoot32BitConstants(_healthBarRoot->GetBRootParamIndex(0), 5, &bar, 0);
				    cmd->RSSetViewports(1, &viewport);
				    cmd->RSSetScissorRects(1, &scissor);
				    cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
				    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
				    cmd->DrawInstanced(3, 1 + bar.Sparks, 0, 0);
			    },
			    {}, barTarget));
			accesses.emplace_back(barResource, Mode::Read);
			hudTextures.push_back({GameHUD::HealthBar, barResource->D3DResource, Engine::UI::OffscreenFormat});
		}
		Id raster = dependency;
		if (screen.GameDraw)
		{
			raster = Packet(graph, bindings, "Game HUD raster", std::move(accesses),
			    [this, &screen, hudTextures = std::move(hudTextures)](auto* cmd)
			    {
				    screen.GameRasterOwner = _screen->TryRecord(*cmd, screen.Target->GetResource()->D3DResource,
				        screen.GameDraw, hudTextures);
				    if (!screen.GameRasterOwner) throw std::logic_error("Game HUD backend slot is still in flight");
			    }, std::move(rasterDependencies), screen.Target.get());
		}
		if (screen.Draw)
		{
			std::vector<std::pair<GDX12Resource*, Mode>> overlayAccesses{
			    {resource(screen.Target.get()), screen.GameDraw ? Mode::ReadWrite : Mode::Write}};
			std::vector<Engine::UI::TextureBinding> overlayTextures;
			if (preview)
			{
				auto* atlas = resource(preview);
				overlayAccesses.emplace_back(atlas, Mode::Read);
				overlayTextures.push_back({SceneTextureInspector::AtlasId, atlas->D3DResource, preview->GetFormat()});
			}
			raster = Packet(graph, bindings, "Information and debug UI raster", std::move(overlayAccesses),
			    [this, &screen, overlayTextures = std::move(overlayTextures)](auto* cmd)
			    {
				    screen.RasterOwner = _screen->TryRecord(*cmd, screen.Target->GetResource()->D3DResource,
				        screen.Draw, overlayTextures, !screen.GameDraw);
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
std::vector<Access> ScenePostUI::AppendPosters(Graph& graph, std::vector<ID3D12Resource*>& bindings)
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
	std::vector<Access> outputs;
	for (size_t index = 0; index < BistroUIPosters::Count; ++index)
	{
		const auto images = _posters->NativeTargets(index);
		if (images.empty())
		{
			continue;
		}
		auto* effect = images[0];
		auto* target = images[1];
		auto* pass = &_posters->RasterPass(index);
		std::vector<std::pair<GDX12Resource*, Mode>> accesses{
		    {effect->GetResource(), index == 2 ? Mode::Read : Mode::ReadWrite}, {target->GetResource(), Mode::Write}};
		auto* raster = target;
		if (images.size() == 3)
		{
			raster = images[2];
			accesses.emplace_back(raster->GetResource(), Mode::ReadWrite);
		}
		const char* names[] = {"UI neon raster / Gaussian glow", "UI ASTRA raster / Gaussian slider",
		                       "UI cloud texture raster"};
		Packet(
		    graph, bindings, names[index], std::move(accesses), [pass](auto* cmd) { pass->Execute(cmd); }, {}, raster);
		const auto targetId =
		    NativeWork::ResourceFor(graph, bindings, target->GetResource()->D3DResource.Get(), "poster material");
		outputs.push_back({targetId, Mode::Read, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE});
	}
	return outputs;
}
} // namespace BistroRender

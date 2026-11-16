#include "ScenePostUI.h"
#include "App.Base/Modules/RenderModule.h"
#include "App.Base/Modules/SceneManagerModule.h"
#include "Engine.Core/BenchmarkEngine.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include "Engine.RendererDX12/GDX12SwapChain.h"
#include "Engine.RendererDX12/GDX12FrameGeneration.h"
#include "Engine.RendererDX12/GDX12MotionBlurSettings.h"
#include "Common/GameTimer.h"

namespace Meta
{
#ifdef _DEBUG
namespace
{
AutoConsoleVariable<bool> CollisionView(L"Meta.Debug.Collisions", false, L"Display the existing Jolt collision view.");
}
#endif
ScenePostUI::ScenePostUI(RenderPipelineContext& c, bool opaqueOnly)
    : _resources(c.Primary), _common(c.Common), _swapchain(c.BackBuffer), _slots(c.Primary.FrameConstants.size()), _exports(c.Primary.FrameConstants.size()), _ordinary(opaqueOnly)
{
    _dof.Setup(&_resources, &_common);
    _blur.Setup(&_resources, &_common);
    _blur.SetPassthroughWhenDisabled(opaqueOnly);
    _reconstruction.Setup(&_resources, &_common);
    _output.Setup(&_resources, &_common);
    _posters = std::make_unique<BistroUIPosters>(c.Runtime,
        BenchmarkEngine::GetLocator().GetModule<SceneManagerModule>(), opaqueOnly);
    if (opaqueOnly) return;
#ifdef _DEBUG
    _collision = std::make_unique<BistroCollisionPass>(*BenchmarkEngine::GetLocator().GetModule<SceneManagerModule>());
    _collision->Setup(&_resources, &_common);
#endif
}
ScenePostUI::~ScenePostUI()
{
    _dof.ClearDenendencies();
    _blur.ClearDenendencies();
    _reconstruction.ClearDenendencies();
    _output.ClearDenendencies();
}
bool ScenePostUI::AllowsFrameGeneration() const
{
#ifdef _DEBUG
    return !CollisionView.GetValue();
#else
    return true;
#endif
}
bool ScenePostUI::NeedsResize() const
{
    return _blur.NeedsTileResize();
}
void ScenePostUI::UpdateSettings()
{
#ifdef _DEBUG
    if (_collisionActive != CollisionView.GetValue())
    {
        _collisionActive = CollisionView.GetValue();
        Reset();
    }
#endif
}
void ScenePostUI::PrepareMaterials(double animationSeconds, GDX12Texture* cloudImage)
{
    _posters->SetImageInput(cloudImage);
    _posters->PrepareFrame(_resources, _resources.CurrFrameConstantsIndex,
        animationSeconds, true);
}
void ScenePostUI::Reset()
{
    _common.RequestHistoryReset = _common.ResetUpscalerHistory = true;
}
void ScenePostUI::Resize()
{
    _reconstruction.QueryRenderTargetResolution();
    _pendingExports = {};
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
    std::vector<GDX12RecordingStates::Binding> states;
    for (auto [resource, mode] : resources)
    {
        auto* native = resource->D3DResource.Get();
        const auto id = NativeWork::ResourceFor(graph, bindings, native, name);
        for (const auto& slot : _exports)
            for (const auto& image : slot)
                if (image.Get() == native) graph.Resources[id].Initial = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        const State state = renderTarget && native == renderTarget->GetResource()->D3DResource.Get()
            ? State(D3D12_RESOURCE_STATE_RENDER_TARGET)
            : native->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER
                ? State(D3D12_RESOURCE_STATE_GENERIC_READ) : textureState;
        const auto found = std::find_if(accesses.begin(), accesses.end(), [id](auto a) { return a.Resource == id; });
        if (found == accesses.end()) { accesses.push_back({id, mode, state}); states.emplace_back(native, D3D12_RESOURCE_STATES(state)); }
        else if (mode != Mode::Read) found->Usage = Mode::ReadWrite;
    }
    const auto id = _work.Add(graph, name, accesses, [record, states](auto* cmd) {
        GDX12RecordingStates tracker(states);
        GDX12CommandList borrowed(cmd);
        record(&borrowed);
        if (borrowed.HasSubmissionSignals()) throw std::logic_error("Post/UI cannot submit private queue signals");
        tracker.Finish(cmd);
    }, std::move(dependencies));
    graph.Operations[id].OwnerRecording = true;
    graph.Operations[id].Target = Queue::Direct;
    return id;
}
void ScenePostUI::Append(Graph& graph, std::vector<ID3D12Resource*>& bindings, GDX12Texture* hdr,
    GDX12Texture* depth, GDX12Texture* motion, GDX12Texture* featureMotion, GDX12Texture* target,
    const std::function<void(GDX12CommandList*)>& finish)
{
    if (!hdr || !depth || !motion || !target || hdr->GetWidth() != depth->GetWidth() ||
        hdr->GetHeight() != depth->GetHeight() || motion->GetWidth() != hdr->GetWidth() ||
        motion->GetHeight() != hdr->GetHeight())
        throw std::invalid_argument("Post inputs require one matching HDR/depth/motion render grid");
    _dof.SetInputs({hdr, depth});
    _blur.SetInputs({_dof.GetOutput(0), depth, motion});
    if (!_initialized)
    {
        _dof.Initialize(); _blur.Initialize();
        _initialized = true;
    }
    else
    {
        auto* output = _dof.GetOutput(0)->GetTexture();
        if (_resize || output->GetWidth() != hdr->GetWidth() || output->GetHeight() != hdr->GetHeight())
        { _dof.Resize(); _blur.Resize(); _resize = true; }
    }
    _dof.PrepareRecording();
    const auto resource = [](GDX12Texture* texture) { return texture->GetResource(); };
    auto* dof = _dof.GetOutput(0)->GetTexture();
    const auto dofId = Packet(graph, bindings, "DoF SDK DIRECT recording boundary",
        {{resource(hdr), Mode::Read}, {resource(depth), Mode::Read}, {resource(dof), Mode::Write}},
        [this](auto* cmd) { _dof.RecordCommands(cmd); });
    const auto outputId = AppendReconstruction(graph, bindings, dof, depth, motion, featureMotion, target, {dofId});
    auto* completeMotion = _blur.GetOutput(1)->GetTexture();
#ifdef _DEBUG
    Id displayId = outputId;
    if (CollisionView.GetValue())
    {
        _collision->SetInputs({target});
        if (!_collisionInitialized) { _collision->Initialize(); _collisionInitialized = true; }
        _collision->PrepareRecording();
        std::vector<std::pair<GDX12Resource*, Mode>> access{{resource(target), Mode::Write}};
        if (auto* upload = _collision->UploadResource()) access.emplace_back(upload, Mode::Read);
        displayId = Packet(graph, bindings, "Jolt collision-only display", access,
            [this](auto* cmd) { _collision->RecordCommands(cmd); }, {outputId}, target);
    }
#else
    const Id displayId = outputId;
#endif
    // Export exactly the states promised by the existing FG tagging API. No
    // terminal transition may invalidate eValidUntilPresent resources.
    const auto fgId = Packet(graph, bindings, "frame-generation prepare/tag DIRECT SDK boundary",
        {{resource(depth), Mode::Read}, {resource(completeMotion), Mode::Read}},
        [this, depth, completeMotion](auto* cmd) { _swapchain->GetFrameGeneration().Prepare(cmd, depth, completeMotion, _common); },
        {displayId}, nullptr, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    auto& exports = _pendingExports;
    const std::array<GDX12Texture*, 2> retained{depth, completeMotion};
    for (size_t index = 0; index < retained.size(); ++index)
    {
        auto resource = retained[index]->GetResource()->D3DResource;
        const auto id = NativeWork::ResourceFor(graph, bindings, resource.Get(), "FG retained input");
        graph.Resources[id].Final = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        exports[index] = std::move(resource);
    }
    AppendScreen(graph, bindings, target, finish, fgId);
}
Id ScenePostUI::AppendReconstruction(Graph& graph, std::vector<ID3D12Resource*>& bindings,
    GDX12Texture* hdr, GDX12Texture* depth, GDX12Texture* motion, GDX12Texture* featureMotion,
    GDX12Texture* target, std::vector<Id> dependencies)
{
    const auto resource = [](GDX12Texture* texture) { return texture->GetResource(); };
    _blur.PrepareRecording(!_ordinary || (_reconstruction.GetFlags() & RENDER_PASS_FLAG_USE_JITTER));
    auto* color = _blur.GetOutput(0)->GetTexture();
    auto* completeMotion = _blur.GetOutput(1)->GetTexture();
    _reconstruction.SetInputs({depth, completeMotion, color});
    if (_reconstruction._needsInitialize) _reconstruction.Initialize();
    else if (_resize) _reconstruction.Resize();
    _reconstruction.LinkOutput();
    _resize = false;
    _output.SetInputs({_reconstruction.GetOutput(0), target});
    if (!_outputInitialized) { _output.Initialize(); _outputInitialized = true; }

    if (!_ordinary || _blur._prepareMotion)
    {
        auto& camera = _resources.FrameConstants[_resources.CurrFrameConstantsIndex]->CameraCB->GetResource();
        std::vector<std::pair<GDX12Resource*, Mode>> accesses{{resource(hdr), Mode::Read},
            {resource(depth), Mode::Read}, {resource(motion), Mode::Read}, {&camera, Mode::Read},
            {resource(_blur._motion.get()), Mode::ReadWrite}, {resource(_blur._velocity.get()), Mode::ReadWrite}};
        if (!_ordinary || _blur._preparedBlur)
            for (auto* image : {_blur._color.get(), _blur._tileMax.get(), _blur._neighborMax.get()})
                accesses.emplace_back(resource(image), Mode::ReadWrite);
        if (featureMotion) accesses.emplace_back(resource(featureMotion), Mode::Read);
        const auto blur = Packet(graph, bindings, "motion preparation / blur DIRECT boundary", std::move(accesses),
            [this, featureMotion](auto* cmd) { _blur.RecordCommands(cmd, featureMotion); }, std::move(dependencies));
        dependencies = {blur};
    }
    auto* reconstructed = _reconstruction.GetOutput(0)->GetTexture();
    const bool native = _reconstruction.GetStatus().Effective == GDX12UpscalerProvider::Native;
    if (!native)
    {
        std::vector<std::pair<GDX12Resource*, Mode>> accesses{{resource(color), Mode::Read},
            {resource(reconstructed), Mode::ReadWrite}};
        if (_reconstruction.GetFlags() & RENDER_PASS_FLAG_USE_JITTER)
        {
            accesses.emplace_back(resource(depth), Mode::Read);
            accesses.emplace_back(resource(completeMotion), Mode::Read);
        }
        const auto reconstruction = Packet(graph, bindings, "reconstruction provider DIRECT SDK boundary",
            std::move(accesses),
            [this](auto* cmd) { _reconstruction.Execute(cmd); }, std::move(dependencies));
        dependencies = {reconstruction};
    }
    return Packet(graph, bindings, "output HDR display transform", {{resource(reconstructed), Mode::Read},
        {resource(target), Mode::Write}}, [this, native](auto* cmd) {
            _output.Execute(cmd);
            if (native) _reconstruction.Execute(cmd);
        }, std::move(dependencies), target);
}
void ScenePostUI::AppendScreen(Graph& graph, std::vector<ID3D12Resource*>& bindings,
    GDX12Texture* target, const std::function<void(GDX12CommandList*)>& finish, Id dependency)
{
    const auto resource = [](GDX12Texture* texture) { return texture->GetResource(); };
    auto& screen = _slots.at(_resources.CurrFrameConstantsIndex);
    screen.RasterOwner.reset(); screen.CompositeOwner.reset();
    screen.Draw = Engine::UI::CaptureScreenFrame();
    Id screenId = dependency;
    if (screen.Draw)
    {
        if (!_screen)
        {
            _screen = std::make_unique<Engine::UI::OffscreenRenderer>(*_resources.Device, UINT(_slots.size()), true);
            _composite = std::make_unique<Engine::UI::ScreenComposite>(*_resources.Device);
        }
        if (!screen.Target)
        {
            screen.Target = std::make_unique<GDX12Texture>();
            GDX12RenderHelpers::InitializeRenderTarget(*screen.Target, _resources, _common.WindowWidth,
                _common.WindowHeight, Engine::UI::OffscreenFormat, {0, 0, 0, 0});
        }
        const auto raster = Packet(graph, bindings, "Engine.UI explicit font upload / offscreen raster boundary",
            {{resource(screen.Target.get()), Mode::Write}}, [this, &screen](auto* cmd) {
                screen.RasterOwner = _screen->TryRecord(*cmd, screen.Target->GetResource()->D3DResource, screen.Draw);
                if (!screen.RasterOwner) throw std::logic_error("Screen UI backend slot is still in flight");
            }, {dependency}, screen.Target.get());
        screenId = Packet(graph, bindings, "Engine.UI screen composition", {{resource(screen.Target.get()), Mode::Read},
            {resource(target), Mode::ReadWrite}}, [this, &screen, target](auto* cmd) {
                screen.CompositeOwner = _composite->Record(*cmd, screen.Target->GetResource()->D3DResource,
                    target->GetResource()->D3DResource);
            }, {raster}, target);
    }
    Packet(graph, bindings, "frame end / PRESENT handoff", {{resource(target), Mode::ReadWrite}}, finish,
        {screenId}, nullptr, D3D12_RESOURCE_STATE_PRESENT);
}
void ScenePostUI::AppendOpaque(Graph& graph, std::vector<ID3D12Resource*>& bindings,
    GDX12Texture* hdr, GDX12Texture* depth, GDX12Texture* motion, GDX12Texture* featureMotion,
    GDX12Texture* target, const std::function<void(GDX12CommandList*)>& finish)
{
    if (!hdr || !depth || !motion || !target || hdr->GetWidth() != depth->GetWidth() ||
        hdr->GetHeight() != depth->GetHeight() || hdr->GetWidth() != motion->GetWidth() ||
        hdr->GetHeight() != motion->GetHeight())
        throw std::invalid_argument("Ordinary post requires matching HDR/depth/motion grids");
    _dof.SetInputs({hdr, depth});
    std::vector<Id> dependencies;
    if (_dof.IsActive())
    {
        if (!_dof._output.IsInitialized()) _dof.Initialize();
        else if (_resize || _dof._output.GetWidth() != hdr->GetWidth() ||
            _dof._output.GetHeight() != hdr->GetHeight()) _dof.Resize();
        _dof.PrepareRecording();
        auto* dof = _dof.GetOutput(0)->GetTexture();
        dependencies.push_back(Packet(graph, bindings, "DoF SDK DIRECT recording boundary",
            {{hdr->GetResource(), Mode::Read}, {depth->GetResource(), Mode::Read}, {dof->GetResource(), Mode::Write}},
            [this](auto* cmd) { _dof.RecordCommands(cmd); }));
        hdr = dof;
    }
    else _dof.PrepareRecording(); // Notify temporal SR even when no DoF packet is needed.
    _blur.SetInputs({hdr, depth, motion});
    if (!_initialized) { _blur.Initialize(); _initialized = true; }
    else if (_resize) _blur.Resize();
    const auto output = AppendReconstruction(graph, bindings, hdr, depth, motion, featureMotion, target, std::move(dependencies));
    AppendScreen(graph, bindings, target, finish, output);
}
void ScenePostUI::AppendPosters(Graph& graph, std::vector<ID3D12Resource*>& bindings, bool record)
{
    for (Id id = 0; id < bindings.size(); ++id)
        for (const auto& slot : _exports)
            for (const auto& image : slot)
                if (image.Get() == bindings[id])
                {
                    graph.Resources[id].Initial = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                    // The diagnostic serial scene may omit post entirely. Keep
                    // imported SDK state valid until the displayed plan uses it.
                    graph.Resources[id].Final = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                }
    const Id consumers = Id(graph.Operations.size());
    for (size_t index = 0; index < BistroUIPosters::Count; ++index)
    {
        const auto images = _posters->NativeTargets(index);
        if (images.empty()) continue;
        auto* effect = images[0]; auto* target = images[1];
        auto* pass = &_posters->RasterPass(index);
        if (record) {
            std::vector<std::pair<GDX12Resource*, Mode>> accesses{
                {effect->GetResource(), index == 2 ? Mode::Read : Mode::ReadWrite},
                {target->GetResource(), Mode::Write}};
            auto* raster = target;
            if (images.size() == 3) {
                raster = images[2];
                accesses.emplace_back(raster->GetResource(), Mode::ReadWrite);
            }
            const char* names[] = {"UI neon raster / Gaussian glow", "UI ASTRA raster / Gaussian slider", "UI cloud texture raster"};
            Packet(graph, bindings, names[index], std::move(accesses),
                [pass](auto* cmd) { pass->Execute(cmd); }, {}, raster);
        }
        const auto targetId = NativeWork::ResourceFor(graph, bindings, target->GetResource()->D3DResource.Get(), "poster material");
        // Opaque posters are sampled by GBuffer/probe color shaders, not masked depth or glass.
        for (Id id = 0; id < consumers; ++id)
            if (graph.Operations[id].Name == "scene GBuffer ExecuteIndirect" ||
                graph.Operations[id].Name == "probe HDR capture mesh/sky")
                graph.Operations[id].Accesses.push_back({targetId, Mode::Read, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE});
    }
}
}

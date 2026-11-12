#include "Rendering/BistroRenderPipeline.h"
#include "App.Base/Modules/RenderModule.h"
#include "App.Base/Rendering/CrossAdapterParticles.h"
#include "App.Base/Rendering/RenderShadowRoutes.h"
#include "App.Base/Rendering/CrossAdapterReflections.h"
#include "App.Base/Rendering/CrossAdapterEnvironment.h"
#include "Rendering/UI/BistroUIPosters.h"
#include "BistroUICanvases.h"
#include <stdexcept>
#include "Common/GameTimer.h"
#include "Rendering/Grass/BistroGrassGeometry.h"
#include "Rendering/Grass/BistroGrassRoute.h"
#include "Rendering/Grass/BistroGrassShadowRoute.h"
#include "Rendering/Water/BistroWaterRendering.h"
#include "Engine.RendererDX12/RenderPasses/GDX12GeometryViewPass.h"
#ifdef _DEBUG
#include "App.Base/Modules/PhysicsModule.h"
#include "App.Base/Rendering/CrossAdapterAO.h"
#include "App.Base/Rendering/CrossAdapterShadows.h"
#include "App.Base/Rendering/CrossAdapterUI.h"
#include "Engine.Core/BenchmarkEngine.h"
#include "Engine.UI/UI_test.h"
#include <iomanip>
#include <sstream>
#include <utility>
#endif

namespace
{
#ifdef _DEBUG
    bool ShowCollisions = false;

    class BistroCollisionPass final : public GDX12RenderPass
    {
    public:
        explicit BistroCollisionPass(SceneManagerModule& scenes) : _scenes(scenes) { _numInputs = 1; }

        void RecordRenderGraph(GDX12RenderGraphBuilder& graph) override
        {
            graph.SetName("Jolt collision view");
            graph.Write(_inputs[0], "Swapchain", Lifetime::Dynamic, D3D12_RESOURCE_STATE_RENDER_TARGET);
        }

        void Initialize() override
        {
            GDX12RootSignatureDesc root;
            root.Constants = {20};
            _root = std::make_unique<GDX12RootSignature>(_resources->Device, root);
            auto& compiler = GDX12ShaderCompiler::GetInstance();
            auto vs = compiler.CompileShader(_resources->Device, APP_SHADERS_FOLDER "CollisionView.hlsl", nullptr, "VS", "vs");
            auto ps = compiler.CompileShader(_resources->Device, APP_SHADERS_FOLDER "CollisionView.hlsl", nullptr, "PS", "ps");
            const D3D12_INPUT_ELEMENT_DESC position = {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
                D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
            D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
            desc.pRootSignature = _root->GetRootSignature().Get();
            desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
            desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
            desc.InputLayout = {&position, 1};
            desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
            desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
            auto& blend = desc.BlendState.RenderTarget[0];
            blend.BlendEnable = true;
            blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
            blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
            desc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
            desc.DepthStencilState.DepthEnable = false;
            desc.SampleMask = UINT_MAX;
            desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            desc.NumRenderTargets = 1;
            desc.RTVFormats[0] = _inputs[0]->GetTexture()->GetFormat();
            desc.SampleDesc.Count = 1;
            auto device = _resources->Device->GetDevice();
            ThrowIfFailed(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&_fill)));
            desc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
            ThrowIfFailed(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&_wire)));
            _uploads.resize(_resources->FrameConstants.size());
        }

        void Execute(GDX12CommandList* commands) override
        {
            auto* target = _inputs[0]->GetTexture();
            GDX12_SCOPED_EVENT(commands, "Jolt collision view", Colors::Lime);
            commands->ResourceBarrier({target->GetResource()->GetRenderTargetBarrier()});
            commands->ClearRenderTargetView(target);
            _statics.clear();
            _moving.clear();
            auto physics = BenchmarkEngine::GetLocator().GetModule<PhysicsModule>();
            if (auto* world = _scenes.GetWorld(); world && physics)
                physics->CollectCollisionTriangles(*world, _statics, _moving);

            const auto count = _statics.size() + _moving.size();
            if (count != 0)
            {
                if (count > UINT_MAX / sizeof(Vector3)) throw std::length_error("Collision view vertex buffer is too large");
                // RenderModule has already waited for the current frame slot's fence.
                auto& upload = _uploads.at(_resources->CurrFrameConstantsIndex);
                if (!upload || upload->GetElementCount() < count)
                    upload = std::make_unique<GDX12UploadBuffer<Vector3>>(_resources->Device, static_cast<UINT>(count), EBufferType::Upload, false);
                const auto staticCount = static_cast<UINT>(_statics.size());
                const auto movingCount = static_cast<UINT>(_moving.size());
                if (staticCount) upload->CopyData(0, _statics.data(), staticCount);
                if (movingCount) upload->CopyData(staticCount, _moving.data(), movingCount);

                commands->SetRenderTargets({target}, nullptr);
                commands->SetViewport(target->GetViewport());
                commands->SetScissorRect(target->GetScissorRect());
                commands->SetGraphicsRootSignature(_root.get());
                commands->SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                auto list = commands->GetCommandList();
                const D3D12_VERTEX_BUFFER_VIEW buffer = {upload->GetElementAddress(0),
                    static_cast<UINT>(count * sizeof(Vector3)), sizeof(Vector3)};
                list->IASetVertexBuffers(0, 1, &buffer);
                commands->InvalidateGeometryBuffer();
                const auto viewProjection = _commonData->CameraView * _commonData->CameraViewToClip;
                list->SetGraphicsRoot32BitConstants(0, 16, &viewProjection, 0);
                const auto draw = [&](const Vector4& tint, UINT start, UINT size)
                {
                    list->SetGraphicsRoot32BitConstants(0, 4, &tint, 16);
                    commands->DrawInstanced(size, 1, start, 0);
                };
                commands->SetPipelineState(_fill);
                if (staticCount) draw({0.10f, 1.0f, 0.22f, 0.27f}, 0, staticCount);
                if (movingCount)
                {
                    draw({1.0f, 0.06f, 0.08f, 0.12f}, staticCount, movingCount);
                    commands->SetPipelineState(_wire);
                    draw({1.0f, 0.13f, 0.16f, 0.50f}, staticCount, movingCount);
                }
            }
            commands->ResourceBarrier({target->GetResource()->GetPresentBarrier()});
        }

    private:
        SceneManagerModule& _scenes;
        std::unique_ptr<GDX12RootSignature> _root;
        ComPtr<ID3D12PipelineState> _fill, _wire;
        std::vector<std::unique_ptr<GDX12UploadBuffer<Vector3>>> _uploads;
        std::vector<Vector3> _statics, _moving;
    };
#endif

    class GrassFramePreparationPass final : public GDX12RenderPass
    {
    public:
        GrassFramePreparationPass(BistroGrassGeometry& grass, const bool& available) : _grass(grass), _available(available) {}
        void RecordRenderGraph(GDX12RenderGraphBuilder& graph) override
        { graph.SetName("Bistro grass simulation"); graph.AsyncCompute(); graph.Read(SharedResource::Scene); graph.HasSideEffects(); }
        bool IsAsyncComputeEnabled() const override { return GetGrassWindControls().AsyncCompute; }
        void Execute(GDX12CommandList* commands) override { if (_available) _grass.PrepareGPU(commands); }
        std::vector<GDX12RenderGraph::BufferAccess> GetBufferAccesses() const override
        { return _available ? _grass.PreparationAccesses() : std::vector<GDX12RenderGraph::BufferAccess>{}; }
    private:
        BistroGrassGeometry& _grass;
        const bool& _available;
    };
    class BistroGeometryPass final : public GDX12RenderPass
    {
    public:
        BistroGeometryPass(GDX12DeviceResources& resources, BistroGrassGeometry& grass, const bool& grassAvailable, GDX12GeometryViewPass& pass,
            int part = 0, const BistroUIPosters* posters = nullptr)
            : _resources(resources), _grass(grass), _grassAvailable(grassAvailable), _pass(pass), _part(part), _posters(posters)
        { _pass.SetSplitGeometry(true); }
        void RecordRenderGraph(GDX12RenderGraphBuilder& graph) override
        {
            _pass.RecordRenderGraph(graph);
            if (_posters) _posters->DeclareMaterialReads(graph);
            if (_part == 1) graph.SetName("Bistro depth meshes");
            if (_part == 2) graph.SetName("Bistro depth grass");
        }
        void Execute(GDX12CommandList* commands) override
        {
            if (_part != 2) _pass.BeginGeometry(commands);
            const auto capacity = _pass.GetGeometryViewCapacity();
            for (UINT slot = 0; slot < capacity; ++slot)
                if (const auto* prepared = _pass.GetGeometryView(slot))
                {
                    if (_part != 2) _pass.ExecuteGeometryView(commands, slot);
                    if (_part == 1 || !_grassAvailable) continue;
                    for (UINT patch = 0; patch < _grass.PatchCount(); ++patch)
                    {
                        auto view = *prepared;
                        view.IncludeApplicationGeometry = true;
                        view.Source = &_grass;
                        view.BatchOffset = patch;
                        view.BatchCount = 1;
                        if (!_grass.Expand(commands, view, patch)) continue;
                        _pass.BindGeometryViewTargets(commands, slot);
                        _resources.EmitGeometry(commands, view, {});
                    }
                }
            if (_part != 1) _pass.EndGeometry(commands);
        }
        std::vector<GDX12RenderGraph::BufferAccess> GetBufferAccesses() const override
        {
            auto accesses = _pass.GetBufferAccesses();
            const auto capacity = _part == 1 || !_grassAvailable ? 0 : _pass.GetGeometryViewCapacity();
            for (UINT slot = 0; slot < capacity; ++slot)
                if (const auto* view = _pass.GetGeometryView(slot))
                    for (UINT patch = 0; patch < _grass.PatchCount(); ++patch)
                    {
                        const auto expansion = _grass.ExpansionAccesses(*view, patch);
                        accesses.insert(accesses.end(), expansion.begin(), expansion.end());
                    }
            return accesses;
        }
    private:
        GDX12DeviceResources& _resources;
        BistroGrassGeometry& _grass;
        const bool& _grassAvailable;
        GDX12GeometryViewPass& _pass;
        int _part; // 0 whole view, 1 meshes before join, 2 grass and final publication
        const BistroUIPosters* _posters;
    };
}

struct BistroRenderPipeline::ReflectionGrass
{
    std::optional<BistroGrassGeometry::ShadowScene> Scene;
    GDX12GrassFieldProducer::SnapshotLayout Layout;
    std::shared_ptr<GDX12Resource> Source;
    GDX12Submission::TransferVersion SourceVersion{};
    std::shared_ptr<const void> Owner;
    CrossAdapterWork::View Remote;
    CrossAdapterWork::Channel Channel;
    std::unique_ptr<GDX12GrassRenderer> Renderer;
    bool Prepared = false;
};

BistroRenderPipeline::BistroRenderPipeline(std::shared_ptr<SceneManagerModule> scenes) : _scenes(std::move(scenes)) {}
BistroRenderPipeline::~BistroRenderPipeline() = default;

std::vector<BistroQualitySearch::TechniqueMeasurement> BistroRenderPipeline::GetQualityMeasurements() const
{
    std::vector<BistroQualitySearch::TechniqueMeasurement> result(2);
    const auto water = _water->GetMGPUWaterDiagnostics();
    const auto grass = _grassRoute->GetDiagnostics();
    const auto grassControls = GetGrassWindControls();
    const auto localWater = _water->Surface.GetLocalSimulationStatus();
    const auto localGrass = _grass->GetLocalSimulationStatus();
    result[0] = {GetFluidControls().Enabled && (localWater.Active || water.Requested), water.Requested, water.SecondaryOwns,
        !water.Requested || water.SourceAgeSeconds >= 0,
        water.SecondaryOwns ? water.SourceAgeSeconds * 1000 : localWater.AgeMs,
        water.SecondaryOwns ? (std::max)(0.0, water.Desired - water.Completed) : localWater.DebtSeconds,
        water.Unrepresented + localWater.MissingSeconds + (water.ResetRequired || water.FirstMissingInput ? 1 : 0), water.SolverInvocations};
    result[1] = {grassControls.GrassEnabled && grassControls.GrassQuality > 0 && _grass->PatchCount() > 0, GetGrassMGPUControls().Enabled,
        grass.Version.IsValid(), grass.LocalAuthoritative || grass.Version.IsValid(), grass.AgeMs + grass.WindDebtSeconds * 1000,
        grass.PendingSeconds + grass.WindDebtSeconds,
        grass.MissingSeconds + (grass.ResetRequired ? 1 : 0), grass.Sequence};
    if (!grass.Version.IsValid())
    {
        result[1].Ready = localGrass.Ready;
        result[1].SourceAgeMs = localGrass.AgeMs;
        result[1].SimulationDebtSeconds = localGrass.DebtSeconds;
        result[1].MissingSeconds += localGrass.MissingSeconds;
    }
    result[0].Name = "water";
    result[1].Name = "grass";
    result[0].ExportBytes = water.ExportBytes; result[0].ImportBytes = water.ImportBytes;
    result[1].ExportBytes = grass.ExportBytes; result[1].ImportBytes = grass.ImportBytes;
    BistroQualitySearch::TechniqueMeasurement particle;
    particle.Name = "particles";
    particle.Active = GetParticleSettings().Enabled && GetParticleSettings().SimulationEnabled && !_resources->ParticleEmitters.empty();
    particle.RequestedSecondary = GetParticleMGPUControls().Secondary;
    if (const auto* route = _passes->GetParticleRoute())
    {
        const auto& state = route->GetDiagnostics();
        particle.EffectiveSecondary = state.Secondary;
        particle.Ready = state.Ready;
        particle.SourceAgeMs = state.AgeMs;
        particle.SimulationDebtSeconds = state.Debt;
        particle.WaitMs = state.WaitMs;
        particle.ExportBytes = particle.ImportBytes = state.InputBytes + state.OutputBytes;
    }
    else particle.Ready = !particle.RequestedSecondary;
    result.push_back(particle);
    const auto shadow = [&](bool sun, GDX12LocalShadowKey key, bool active, double source)
    {
        BistroQualitySearch::TechniqueMeasurement sample;
        sample.Name = sun ? "sun-shadow" : "local-shadow-" + std::to_string(key.EntityId);
        sample.Active = active;
        sample.RequestedSecondary = ReadShadowUpdatePolicy(sun ? 0 : key.EntityId).Secondary;
        if (const auto* routes = _passes->GetShadowRoutes())
            for (const auto& unit : routes->GetDiagnostics())
                if (unit.Sun == sun && (sun || unit.Light == key))
                {
                    source = unit.SourceTime;
                    sample.EffectiveSecondary = unit.EffectiveSecondary;
                    sample.ExportBytes = unit.Transfer.ExportBytes;
                    sample.ImportBytes = unit.Transfer.ImportBytes;
                    sample.SourceSequence = unit.Transfer.Produced;
                }
        sample.Ready = source > 0;
        sample.SourceAgeMs = sample.Ready ? (std::max)(0.0, (ShadowSourceTimeSeconds() - source) * 1000) : 0;
        result.push_back(sample);
    };
    shadow(true, {}, (_resources->Lighting.SunShadowFlags & 1) != 0 && _resources->ShadowQuality.SunResolution != 0,
        _passes->SunShadow.GetDisplayedSourceTime());
    for (const auto& light : _resources->LocalLights)
    {
        const auto key = GetLocalShadowKey(light);
        shadow(false, key, (light.ShadowFlags & 1) != 0 && _resources->ShadowQuality.LocalResolution != 0,
            _passes->LocalShadow.GetDisplayedSourceTime(key));
    }
    const auto& reflection = _passes->Reflections.GetStatistics();
    const auto ready = _passes->Reflections.GetReadiness();
    BistroQualitySearch::TechniqueMeasurement probe;
    probe.Name = "reflection-probes";
    probe.Active = GetReflectionSettings().Enabled && ready.second != 0;
    probe.RequestedSecondary = reflection.RequestedSecondaryFaces != 0;
    probe.EffectiveSecondary = reflection.EffectiveSecondaryFaces != 0;
    probe.Ready = ready.first == ready.second;
    probe.MissingSeconds = reflection.RequestedSecondaryFaces != reflection.EffectiveSecondaryFaces ? 1 : 0;
    probe.FullSourceAgeMs = reflection.MaxFaceAgeSeconds * 1000;
    probe.SourceAgeMs = reflection.AdditionalLagSeconds * 1000;
    if (const auto* route = _passes->GetReflectionRoute())
    {
        const auto diagnostics = route->GetDiagnostics();
        probe.WaitMs = diagnostics.WaitMs;
        probe.ExportBytes = probe.ImportBytes = diagnostics.SceneBytes + diagnostics.ResultBytes;
    }
    result.push_back(probe);
    for (const auto& age : _passes->Reflections.GetFaceAges())
    {
        auto face = probe;
        face.Name = "reflection-" + std::to_string(age.Probe) + "-face-" + std::to_string(age.Face);
        face.FullSourceAgeMs = age.Seconds * 1000;
        face.WaitMs = 0;
        face.ExportBytes = face.ImportBytes = 0;
        result.push_back(std::move(face));
    }
    return result;
}

std::optional<D3D12_RESOURCE_DESC> BistroRenderPipeline::GetReflectionShadowDescription() const
{
    if (!_passes) return std::nullopt;
    auto* texture = _passes->LocalShadow.GetOutput(0)->GetTexture();
    if (!texture || !texture->IsInitialized()) return std::nullopt;
    return texture->GetResource()->D3DResource->GetDesc();
}

bool BistroRenderPipeline::IsCollisionViewEnabled()
{
#ifdef _DEBUG
    return ShowCollisions;
#else
    return false;
#endif
}

#ifdef _DEBUG
void BistroRenderPipeline::DrawDebugControls(const RenderModule& render)
{
    using namespace Engine::UI;
    if (!DebugControlsVisible()) return;

    const auto placement = [](bool secondary) { return secondary ? "secondary" : "primary"; };
    const auto freshness = [](bool exact) { return exact ? "Exact" : "Latest"; };
    const auto fallback = [&](std::string_view reason) -> std::string_view
    {
        const auto& mgpu = render.GetMGPUStatus();
        if (!mgpu.Requested) return "MGPU disabled";
        if (!mgpu.FallbackReason.empty()) return mgpu.FallbackReason;
        return reason;
    };
    const auto line = [](std::string_view name, std::string_view requested, std::string_view effective,
        std::string_view mode, double ageMs = -1, bool overAge = false, std::string_view reason = {})
    {
        std::ostringstream text;
        text << name << " | requested " << requested << " | active " << effective << " | " << mode;
        if (ageMs >= 0) text << " | " << std::fixed << std::setprecision(1) << ageMs << " ms";
        if (overAge) text << " | OVER AGE";
        if (!reason.empty()) text << " | " << reason;
        return text.str();
    };

    const auto water = _water->GetMGPUWaterDiagnostics();
    const auto grass = _grassRoute->GetDiagnostics();
    const auto grassControls = GetGrassMGPUControls();
    const auto cloud = _cloudEnvironment->GetDiagnostics();
    using CloudRoute = CrossAdapterEnvironment::Route;
    const auto cloudPlacement = [](CloudRoute route)
    {
        switch (route)
        {
        case CloudRoute::SecondaryCompute: return "secondary COMPUTE";
        case CloudRoute::LocalCompute: return "primary COMPUTE";
        default: return "primary DIRECT";
        }
    };
    ConsoleModule console;
    const bool cloudExact = !console.FindConsoleVariable(L"Render.Clouds.Latest").GetBool();
    const bool aoRequested = IsMultiGPUAmbientOcclusionEnabled();
    const bool aoActive = _passes->GetLastAmbientOcclusionFinalize().IsValid();
    const auto* shadows = _passes->GetShadowRoutes();
    using ShadowScope = GDX12ShadowSelection::Mode;
    const auto scope = static_cast<ShadowScope>(console.FindConsoleVariable(L"Render.Shadows.MultiGPU.Selection").GetInt());
    const bool shadowsRequested = IsMultiGPUShadowsEnabled();
    const bool sunRequested = shadowsRequested && (scope == ShadowScope::All || scope == ShadowScope::Sun);
    const bool localRequested = shadowsRequested && scope != ShadowScope::Sun;
    const bool sunActive = shadows && shadows->GetTrace(GDX12ShadowAtlas::Sun);
    const bool localActive = shadows && shadows->GetTrace(GDX12ShadowAtlas::Local);
    const auto exactLine = [&](const char* name, bool requested, bool active)
    {
        return line(name, placement(requested), placement(active), "Exact", -1, false,
            requested && !active ? fallback("No secondary receipt") : std::string_view{});
    };

    const auto* secondary = render.GetMGPUDevices().Secondary;
    const auto uiLine = [&](const char* name, CanvasId surface)
    {
        const auto status = render.GetUIPlacementStatus(surface);
        const bool active = secondary && status.ActiveAdapter == secondary->GetSubmissionAdapterId();
        const bool exact = status.Mode == GDX12Submission::SnapshotReadMode::ExactVersion;
        return line(name, placement(status.Requested), placement(active), freshness(exact),
            status.Displayed.IsValid() ? status.Age.Milliseconds : -1, status.Age.OverAge, status.Fallback);
    };

    if (DebugButton(ShowCollisions ? "Show scene###CollisionView" : "Show Jolt collisions###CollisionView", {
        exactLine("AO", aoRequested, aoActive),
        exactLine("Sun shadows", sunRequested, sunActive),
        exactLine("Local shadows", localRequested, localActive),
        line("Water", placement(water.Requested), placement(water.SecondaryOwns), freshness(water.Exact),
            water.SnapshotAgeSeconds >= 0 ? water.SnapshotAgeSeconds * 1000 : -1, water.OverAge,
            water.Requested && !water.SecondaryOwns ? fallback(water.Status) : std::string_view(water.Status)),
        line("Grass", placement(grassControls.Enabled), placement(grass.Version.IsValid()), freshness(grassControls.Exact),
            grass.Version.IsValid() ? grass.AgeMs : -1, grass.OverAge,
            grassControls.Enabled && !grass.Version.IsValid() ? fallback("No secondary field selected") : std::string_view{}),
        line("Clouds", cloudPlacement(cloud.Requested), cloudPlacement(cloud.Effective), freshness(cloudExact),
            cloud.DisplayedInvocation ? cloud.DisplayAge.Milliseconds : -1, cloud.DisplayAge.OverAge,
            cloud.Requested == CloudRoute::SecondaryCompute && cloud.Effective != cloud.Requested ?
                fallback("No secondary snapshot selected") : std::string_view{}),
        uiLine("UI screen", CanvasId::Invalid),
        uiLine("UI Atrium", BistroUICanvases::Atrium),
        uiLine("UI Terrace", BistroUICanvases::Terrace)
    }))
        ToggleCollisionView();
}

void BistroRenderPipeline::ToggleCollisionView()
{
    ShowCollisions = !ShowCollisions;
}
#endif

void BistroRenderPipeline::Initialize(RenderPipelineContext& context)
{
    if (_passes) throw std::logic_error("Render pipeline is already initialized");
    _common = &context.Common;
    _resources = &context.Primary;
    _earlyAO = GetAmbientOcclusionExecutionPoint() == 0;
    _grass = std::make_unique<BistroGrassGeometry>(_scenes, context.Runtime);
    _grassRoute = std::make_unique<BistroGrassRoute>();
    _grassShadows = std::make_unique<BistroGrassShadowRoute>();
    context.Primary.GeometrySources.push_back(_grass.get());
    _water = std::make_unique<BistroWaterRendering>(_scenes);
    _posters = std::make_unique<BistroUIPosters>(context.Runtime, _scenes);
    _passes = std::make_unique<CommonRenderPasses>(context.BackBuffer, GDX12WBOITTransparencyPass::DepthPartition::Near);
    _passes->Composition.SetConditionalTransparency(&_passes->Transparency);
    _passes->Composition.SetApplyOpaqueAerialPerspective(false);
    auto& passes = _passes->Bindings.PrimaryPasses;
    passes.insert(passes.begin(), &_water->Simulation);
    passes.insert(std::find(passes.begin(), passes.end(), &_passes->AO), &_water->Surface);
    passes.insert(std::find(passes.begin(), passes.end(), &_passes->Transparency),
        { &_water->Transparency, &_water->Composition, &_water->Resolve });
    _water->Transparency.SetFlag(RENDER_PASS_FLAG_USE_DOWNSCALED_RESOLUTION, true);
    _water->Composition.SetFlag(RENDER_PASS_FLAG_USE_DOWNSCALED_RESOLUTION, true);
    _passes->Setup(context);
    LinkInputs({ context.BackBuffer });
    _passes->Initialize();
    _reflectionGrass = std::make_unique<ReflectionGrass>();
    if (auto* route = _passes->GetReflectionRoute())
        route->SetApplication(
            [this](CrossAdapterWork& work)
            {
                const auto& state = *_reflectionGrass;
                return state.Source ? std::vector<CrossAdapterWork::Use>{{work.Identify(state.Source, *_resources->Device),
                    GDX12RenderGraph::Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, state.SourceVersion}} :
                    std::vector<CrossAdapterWork::Use>{};
            },
            [this](GDX12DeviceResources& resources, GDX12ReflectionProbePass& capture, CrossAdapterWork& work, UINT64& copiedBytes)
            {
                auto& state = *_reflectionGrass;
                state.Prepared = false;
                if (!state.Scene || state.Scene->Patches.empty()) return std::vector<CrossAdapterWork::Use>{};
                if (!state.Source) throw std::logic_error("Reflection grass has no corresponding field snapshot");
                const auto bytes = state.Source->D3DResource->GetDesc().Width;
                if (!state.Remote.Resource || state.Remote.Resource->D3DResource->GetDesc().Width != bytes)
                    state.Remote = work.Allocate(*resources.Device, bytes);
                copiedBytes += work.Copy(state.Channel, *_resources->Device, *resources.Device,
                    {{work.Identify(state.Source, *_resources->Device), state.Remote, UINT_MAX, UINT_MAX, state.SourceVersion}});
                auto snapshot = std::make_shared<GDX12GrassFieldSnapshot>(*resources.SRV_UAV_Heap,
                    *state.Remote.Resource, state.Layout, state.Remote.Resource);
                if (!state.Renderer) state.Renderer = std::make_unique<GDX12GrassRenderer>(&resources);
                std::vector<GDX12GrassInstance> instances;
                for (const auto& patch : state.Scene->Patches) instances.push_back(patch.Instance);
                const GDX12GrassRenderer::DisplayedFields fields{snapshot, snapshot, true};
                state.Renderer->BeginFrame(*_common, std::move(instances), state.Scene->Wind, state.Scene->Controls, {}, 0, &fields);
                for (UINT slot = 0; slot < capture.GetGeometryViewCapacity(); ++slot)
                    if (const auto* view = capture.GetGeometryView(slot))
                        for (UINT patch = 0; patch < state.Renderer->PatchCount(); ++patch) state.Renderer->PrepareView(*view, patch);
                return std::vector<CrossAdapterWork::Use>{{state.Remote, GDX12RenderGraph::Mode::Read,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE}};
            },
            [this](GDX12CommandList& commands, GDX12ReflectionProbePass& capture, UINT slot)
            {
                auto& state = *_reflectionGrass;
                if (!state.Scene || state.Scene->Patches.empty()) return;
                if (!state.Prepared) { state.Renderer->PrepareGPU(&commands); state.Prepared = true; }
                const auto& view = *capture.GetGeometryView(slot);
                for (UINT patch = 0; patch < state.Renderer->PatchCount(); ++patch)
                {
                    if (!state.Renderer->Expand(&commands, view, patch, true)) continue;
                    capture.BindGeometryViewTargets(&commands, slot);
                    auto draw = view;
                    draw.BatchOffset = patch;
                    draw.BatchCount = 1;
                    std::vector<GDX12RenderBatch> batches;
                    state.Renderer->GatherBatches(draw, batches);
                    for (const auto& batch : batches) GDX12EmitRenderBatch(&commands, draw, batch);
                }
            });
    _cloudEnvironment = std::make_unique<CrossAdapterEnvironment>(_passes->Environment, context.Primary,
        context.Runtime.GetMGPUDevices().Secondary, context.Runtime.GetMGPURuntime());
    _passes->UpdateAmbientOcclusionRoute(context);
    _geometryJobs.push_back(std::make_unique<GrassFramePreparationPass>(*_grass, _grassAvailable));
    _geometryJobs.push_back(std::make_unique<BistroGeometryPass>(*_resources, *_grass, _grassAvailable, _passes->Depth, 1, _posters.get()));
    _geometryJobs.push_back(std::make_unique<BistroGeometryPass>(*_resources, *_grass, _grassAvailable, _passes->Depth, 2));
    const auto geometry = GeometryPasses();
    for (size_t i = 1; i < geometry.size(); ++i)
    {
        const bool samplesPoster = geometry[i] == &_passes->Opaque || geometry[i] == &_passes->Reflections;
        _geometryJobs.push_back(std::make_unique<BistroGeometryPass>(*_resources, *_grass, _grassAvailable, *geometry[i], 0,
            samplesPoster ? _posters.get() : nullptr));
    }
}

std::vector<Engine::UI::CanvasId> BistroRenderPipeline::GetWorldCanvasIds() const
{
    return { BistroUICanvases::Atrium, BistroUICanvases::Terrace };
}

void BistroRenderPipeline::ResetWorldHistory()
{
    _passes->ResetWorldHistory();
    _grassRoute->ResetWorld();
    _grass->RestartLocalSolver();
    _grassAvailable = false;
    _cloudEnvironment->CloudSource().Reset();
    if (_resources->ParticleSimulation) _resources->ParticleSimulation->SetInteractionField({});
}

std::pair<size_t, size_t> BistroRenderPipeline::GetWorldProbeReadiness() const
{
    return _passes->Reflections.GetReadiness();
}

void BistroRenderPipeline::PrepareMaterialBindings(RenderPipelineContext& context, Engine::UI::CrossAdapterUI* controller)
{
    const double seconds = context.Common.GameTimer ? context.Common.GameTimer->TotalTime() : 0.0;
    _posterGraphChanged = _posters->PrepareFrame(context.Primary, context.Primary.CurrFrameConstantsIndex, seconds, controller);
}

void BistroRenderPipeline::UpdateFrameSettings(RenderPipelineContext& context)
{
#ifdef _DEBUG
    if (IsCollisionViewEnabled() != _collisionViewActive)
        context.Common.RequestHistoryReset = true;
#endif
    if (_passes->Reconstruction.ApplySettings())
        context.Resize();
}

bool BistroRenderPipeline::PrepareFrame(RenderPipelineContext& context, GDX12CommandList* commands)
{
    bool rebuild = std::exchange(_posterGraphChanged, false);
    const bool showCollisions = IsCollisionViewEnabled();
    _grassShadows->CollectRetired();
    _grassAvailable = _grassRoute->PrepareFrame(context, *_grass, !showCollisions);
    const float delta = context.Common.GameTimer ? context.Common.GameTimer->DeltaTime() : 0;
    _water->PrepareFrame(_water->CollectScene(delta), context.Runtime.GetMGPURuntime(), context.Primary,
        context.Runtime.GetMGPUDevices().Secondary, context.Runtime.GetFrameSubmissionId(), !showCollisions);
    const auto* world = _scenes->GetWorld();
    const double cloudDelta = world && !world->IsPaused() ? delta * world->GetTimeScale() : 0.0;
#ifdef _DEBUG
    rebuild |= std::exchange(_collisionViewActive, showCollisions) != showCollisions;
    if (showCollisions)
    {
        _cloudEnvironment->PrepareFrame(cloudDelta, false);
        return rebuild;
    }
#endif
    const bool early = GetAmbientOcclusionExecutionPoint() == 0;
    rebuild |= early != _earlyAO;
    rebuild |= _passes->UpdateAmbientOcclusionRoute(context);
    _earlyAO = early;
    _water->Surface.PrepareFrame();
    _water->Resolve.PrepareFrame();
    _passes->BindAmbientOcclusion(context.Common, _earlyAO);
    rebuild |= _passes->PrepareFrame(context, commands);
    const bool cloudSplit = _cloudEnvironment->PrepareFrame(cloudDelta);
    rebuild |= std::exchange(_cloudSplit, cloudSplit) != cloudSplit;
    _passes->Bindings.UseAsyncCompute |= _posters->UsesAsyncCompute();
    _passes->Bindings.UseAsyncCompute |= GetFluidControls().AsyncCompute || GetGrassWindControls().AsyncCompute;
    if (_resources->ParticleSimulation)
        _resources->ParticleSimulation->SetInteractionField(_grassAvailable ? _grass->InteractionField() : GDX12ParticleInteractionField{});
    if (auto* particles = _passes->GetParticleRoute())
    {
        const auto* displayed = _grassAvailable ? _grassRoute->GetDisplay() : nullptr;
        particles->SetInteractionVersion(displayed ? displayed->CurrentView.Version : GDX12Submission::TransferVersion{});
    }
    auto& reflectionGrass = *_reflectionGrass;
    reflectionGrass.Scene.reset();
    reflectionGrass.Source.reset();
    reflectionGrass.SourceVersion = {};
    reflectionGrass.Owner.reset();
    if (auto* route = _passes->GetReflectionRoute(); route && route->HasSecondaryWork() && _grassAvailable)
    {
        reflectionGrass.Scene = _grass->CaptureShadowScene(*_resources, false);
        if (reflectionGrass.Scene && !reflectionGrass.Scene->Patches.empty())
        {
            if (const auto* displayed = _grassRoute->GetDisplay())
            {
                reflectionGrass.Layout = displayed->Fields.Current->GetLayout();
                reflectionGrass.Source = displayed->CurrentView.Resource;
                reflectionGrass.SourceVersion = displayed->CurrentView.Version;
                reflectionGrass.Owner = displayed->Fields.Current;
            }
            else
            {
                const auto checkpoint = _grass->PrepareLocalCheckpoint();
                if (!checkpoint) throw std::logic_error("Cannot capture primary grass for secondary reflections");
                reflectionGrass.Layout = checkpoint->Layout;
                reflectionGrass.Source = checkpoint->Packed;
                reflectionGrass.Owner = checkpoint;
            }
        }
    }
    if (!_grassAvailable) return rebuild;
    if (auto* shadows = _passes->GetShadowRoutes())
        _grassShadows->PrepareFrame(context, *_grass, *_grassRoute, *shadows);
    const auto passes = GeometryPasses();
    for (UINT i = 0; i < passes.size(); ++i)
    {
        const auto capacity = passes[i]->GetGeometryViewCapacity();
        for (UINT slot = 0; slot < capacity; ++slot)
            if (const auto* view = passes[i]->GetGeometryView(slot))
                for (UINT patch = 0; patch < _grass->PatchCount(); ++patch) _grass->PrepareView(*view, patch);
    }
    return rebuild;
}

void BistroRenderPipeline::LinkInputs(const RenderPipelineGraphInputs& inputs)
{
    auto& p = *_passes;
    p.Culling.SetInputs({});
    p.Depth.SetInputs({});
    p.ParticleSimulation.SetInputs({ p.Depth.GetOutput(2) });
    p.Reflections.SetInputs({ p.Environment.GetOutput(0), p.Environment.GetOutput(1),
        p.Environment.GetOutput(2), p.Environment.GetOutput(3), p.LocalShadow.GetOutput(0) });
    p.Opaque.SetInputs({ p.Depth.GetOutput(2) });
    p.BindAmbientOcclusion(*_common, _earlyAO);
    p.Lighting.SetInputs({ p.Opaque.GetOutput(0), p.Opaque.GetOutput(3), p.Opaque.GetOutput(4),
        p.Opaque.GetOutput(5), p.Opaque.GetOutput(2), p.SunShadow.GetOutput(0), p.Environment.GetOutput(0),
        p.Environment.GetOutput(1), p.Environment.GetOutput(2), p.Environment.GetOutput(3),
        p.LocalShadow.GetOutput(0), p.AO.GetOutput(0) });
    p.SSR.SetInputs({ p.Lighting.GetOutput(0), p.Opaque.GetOutput(2), p.Opaque.GetOutput(3),
        p.Opaque.GetOutput(4), p.Opaque.GetOutput(1), p.Environment.GetOutput(1), p.Environment.GetOutput(2) });
    auto& water = *_water;
    water.Surface.SetInputs({ p.Opaque.GetOutput(2), p.Opaque.GetOutput(1) });
    water.Transparency.SetInputs({ p.Opaque.GetOutput(2), p.SunShadow.GetOutput(0), p.Environment.GetOutput(0),
        p.Environment.GetOutput(1), p.Environment.GetOutput(2), p.LocalShadow.GetOutput(0), water.Surface.GetOutput(0) });
    water.Composition.SetInputs({ p.SSR.GetOutput(0), water.Transparency.GetOutput(0), water.Transparency.GetOutput(1),
        p.Opaque.GetOutput(2) });
    water.Resolve.SetInputs({ water.Composition.GetOutput(0), p.Environment.GetOutput(1) });
    p.Transparency.SetInputs({ p.Opaque.GetOutput(2), p.SunShadow.GetOutput(0), p.Environment.GetOutput(0),
        p.Environment.GetOutput(1), p.Environment.GetOutput(2), p.LocalShadow.GetOutput(0), water.Surface.GetOutput(0) });
    p.Particles.SetInputs({ water.Surface.GetOutput(0), p.Transparency.GetOutput(0), p.Transparency.GetOutput(1) });
    p.Composition.SetInputs({ water.Resolve.GetOutput(0), p.Transparency.GetOutput(0), p.Transparency.GetOutput(1),
        water.Surface.GetOutput(0) });
    p.Composition.SetParticles(&p.Particles);
    p.DepthOfField.SetInputs({ p.Composition.GetOutput(0), water.Surface.GetOutput(0) });
    p.MotionBlur.SetInputs({ p.DepthOfField.GetOutput(0), water.Surface.GetOutput(0), water.Surface.GetOutput(1) });
    p.Reconstruction.SetInputs({ water.Surface.GetOutput(0), p.MotionBlur.GetOutput(1), p.MotionBlur.GetOutput(0) });
    p.Output.SetInputs({ p.Reconstruction.GetOutput(0), inputs.BackBuffer });
    p.FrameGeneration.SetInputs({ water.Surface.GetOutput(0), p.MotionBlur.GetOutput(1), inputs.BackBuffer });
}

void BistroRenderPipeline::BuildGraph(GDX12RenderGraph& graph, const RenderPipelineGraphInputs& inputs)
{
    _passes->Bindings.LegacyReplacements.clear();
    _passes->Bindings.LegacyReplacements.push_back({ nullptr, {},
        [this](auto tail) { _water->RetireFrame(tail); }, {} });
    _passes->Bindings.LegacyReplacements.push_back({ nullptr, {},
        [this](auto tail) { _grassShadows->FinishFrame(); _grassRoute->RetireFrame(tail); },
        [this](auto& commands) { _grassRoute->RecordTail(commands); } });
#ifdef _DEBUG
    if (_collisionViewActive)
    {
        if (!_collisionPass)
        {
            auto pass = std::make_unique<BistroCollisionPass>(*_scenes);
            pass->Setup(_resources, nullptr, _common);
            pass->SetInputs({inputs.BackBuffer});
            pass->Initialize();
            _collisionPass = std::move(pass);
        }
        graph.AddPass(*_collisionPass);
        return;
    }
#endif
    LinkInputs(inputs);
    auto& p = *_passes;
    // Small latency-sensitive work precedes the long water solve on the single COMPUTE queue.
    const auto grassPreparation = graph.AddPass(*_geometryJobs.at(0));
    _posters->AddPasses(graph);
    if (_posters->HasRoutes())
        for (size_t index = 0; index < BistroUIPosters::Count; ++index)
            p.Bindings.LegacyReplacements.push_back({ &_posters->RasterPass(index),
                [this](const auto& boundary, auto prefix) { return _posters->SubmitReplacement(boundary, prefix); }, {}, {} });
    const auto waterSimulation = graph.AddPass(_water->Simulation);
    graph.AddPass(*_cloudEnvironment);
    if (_cloudSplit)
    {
        graph.SetLegacySplit(*_cloudEnvironment, {}, { p.Environment.GetOutput(0), p.Environment.GetOutput(1),
            p.Environment.GetOutput(2), p.Environment.GetOutput(3) });
        p.Bindings.LegacyReplacements.push_back({ _cloudEnvironment.get(),
            [this](const auto& boundary, auto prefix) { return _cloudEnvironment->Submit(boundary, prefix); }, {}, {} });
    }
    graph.AddPass(p.Culling);
    auto geometryTail = graph.AddPass(*_geometryJobs.at(1));
    const auto addGeometry = [&](UINT index)
    {
        const auto node = graph.AddPass(*_geometryJobs.at(index));
        graph.DependsOn(node, geometryTail);
        geometryTail = node;
    };
    addGeometry(2);
    graph.DependsOn(geometryTail, grassPreparation);
    p.AddParticleSimulation(graph);
    if (_earlyAO) p.AddAmbientOcclusion(graph, _earlyAO);
    if (p.GetShadowRoutes())
        geometryTail = _grassShadows->AddBoundary(graph, geometryTail, p.Bindings);
    addGeometry(3);
    addGeometry(4);
    p.AddShadowRoutes(graph, *_geometryJobs.at(3), *_geometryJobs.at(4));
    addGeometry(5);
    p.AddReflectionRoute(graph, *_geometryJobs.at(5), {&_posters->Output(0), &_posters->Output(1)});
    addGeometry(6);
    if (!_earlyAO) p.AddAmbientOcclusion(graph, _earlyAO);
    const auto waterSurface = graph.AddPass(_water->Surface);
    graph.DependsOn(waterSurface, waterSimulation);
    graph.SetLegacyPrefixBefore(_water->Surface, [this] { return _water->WantsExactBoundary(); });
    p.Bindings.LegacyReplacements.push_back({ &_water->Surface,
        [this](const auto&, auto prefix) { return _water->SubmitExact(prefix); }, {}, {} });
    graph.AddPass(p.Lighting);
    const auto hdr = graph.AddPass(p.SSR);
    const auto farTransparency = graph.AddPass(_water->Transparency);
    graph.DependsOn(farTransparency, hdr);
    graph.AddPass(_water->Composition);
    const auto resolve = graph.AddPass(_water->Resolve);
    const auto nearTransparency = graph.AddPass(p.Transparency);
    graph.DependsOn(nearTransparency, resolve);
    graph.AddPass(p.Particles);
    graph.AddPass(p.Composition);
    graph.AddPass(p.DepthOfField);
    graph.AddPass(p.MotionBlur);
    graph.AddPass(p.Reconstruction);
    graph.AddPass(p.Output);
    graph.AddPass(p.FrameGeneration);
}

void BistroRenderPipeline::Resize(RenderPipelineContext& context)
{
    _passes->Resize(context);
}

const RenderPipelineBindings& BistroRenderPipeline::GetBindings() const
{
    static const RenderPipelineBindings empty;
    return _passes ? _passes->Bindings : empty;
}

std::vector<GDX12GeometryViewPass*> BistroRenderPipeline::GeometryPasses()
{ return { &_passes->Depth, &_passes->SunShadow, &_passes->LocalShadow, &_passes->Reflections, &_passes->Opaque }; }

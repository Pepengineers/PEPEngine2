#include "Rendering/BistroRenderPipeline.h"
#include "Engine.RendererDX12/RenderPasses/GDX12CloudComponent.h"
#include "App.Base/Modules/RenderModule.h"
#include "Rendering/UI/BistroUIPosters.h"
#include <stdexcept>
#include "Common/GameTimer.h"
#include "Rendering/Grass/BistroGrassGeometry.h"
#include "Rendering/Water/BistroWaterRendering.h"
#include "Engine.RendererDX12/RenderPasses/GDX12GeometryViewPass.h"
#ifdef _DEBUG
#include "App.Base/Modules/PhysicsModule.h"
#include "Engine.Core/BenchmarkEngine.h"
#include "Engine.UI/UI_test.h"
#include <utility>
#endif

namespace
{
    AutoConsoleVariable<bool> BistroClouds(L"Render.Clouds.Bistro", false,
        L"Enable the procedural cloud source in Bistro.");
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

BistroRenderPipeline::BistroRenderPipeline(std::shared_ptr<SceneManagerModule> scenes) : _scenes(std::move(scenes)) {}
BistroRenderPipeline::~BistroRenderPipeline() = default;

bool BistroRenderPipeline::IsCollisionViewEnabled()
{
#ifdef _DEBUG
    return ShowCollisions;
#else
    return false;
#endif
}

#ifdef _DEBUG
void BistroRenderPipeline::DrawDebugControls(const RenderModule&)
{
    if (Engine::UI::DebugButton(ShowCollisions ? "Show scene###CollisionView" : "Show Jolt collisions###CollisionView"))
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
    context.Primary.GeometrySources.push_back(_grass.get());
    _water = std::make_unique<BistroWaterRendering>(_scenes);
    _posters = std::make_unique<BistroUIPosters>(context.Runtime, _scenes);
    _clouds = std::make_unique<GDX12CloudComponent>();
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

void BistroRenderPipeline::ResetWorldHistory()
{
    _passes->ResetWorldHistory();
    _clouds->Reset();
    _grassAvailable = false;
    if (_resources->ParticleSimulation) _resources->ParticleSimulation->SetInteractionField({});
}

std::pair<size_t, size_t> BistroRenderPipeline::GetWorldProbeReadiness() const
{
    return _passes->Reflections.GetReadiness();
}

void BistroRenderPipeline::PrepareMaterialBindings(RenderPipelineContext& context)
{
    const double seconds = context.Common.GameTimer ? context.Common.GameTimer->TotalTime() : 0.0;
    _posterGraphChanged = _posters->PrepareFrame(context.Primary, context.Primary.CurrFrameConstantsIndex, seconds);
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
    const float delta = context.Common.GameTimer ? context.Common.GameTimer->DeltaTime() : 0;
    _passes->Environment.SetCloudSource(BistroClouds.GetValue() ? _clouds.get() : nullptr);
    const auto* world = _scenes->GetWorld();
    if (BistroClouds.GetValue()) _clouds->Advance(world && !world->IsPaused() ? delta * world->GetTimeScale() : 0.0);

#ifdef _DEBUG
    const bool showCollisions = IsCollisionViewEnabled();
    rebuild |= std::exchange(_collisionViewActive, showCollisions) != showCollisions;
    if (showCollisions) return rebuild;
#endif
    const bool early = GetAmbientOcclusionExecutionPoint() == 0;
    rebuild |= early != _earlyAO;
    _earlyAO = early;
    _grass->CollectScene(delta);
    _grass->PrepareFrame(context.Primary, context.Common);
    _grassAvailable = true;
    _water->Surface.SetInstances(_water->CollectScene(delta));
    _water->Surface.PrepareFrame();
    _water->Resolve.PrepareFrame();
    _passes->BindAmbientOcclusion(context.Common, _earlyAO);
    if (!_resources->ParticleSimulation) _resources->ParticleSimulation = std::make_unique<GDX12ParticleSimulation>();
    _resources->ParticleSimulation->SetInteractionField(_grass->InteractionField());
    _passes->PrepareFrame(context, commands);
    _passes->Bindings.UseAsyncCompute |= _posters->UsesAsyncCompute();
    _passes->Bindings.UseAsyncCompute |= GetFluidControls().AsyncCompute || GetGrassWindControls().AsyncCompute;
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
#ifdef _DEBUG
    if (_collisionViewActive)
    {
        if (!_collisionPass)
        {
            auto pass = std::make_unique<BistroCollisionPass>(*_scenes);
            pass->Setup(_resources, _common);
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
    graph.AddPass(p.ParticleOffload);
    _posters->AddPasses(graph);
    const auto waterSimulation = graph.AddPass(_water->Simulation);
    graph.AddPass(p.Environment);
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
    if (_earlyAO) p.AddAmbientOcclusion(graph);
    addGeometry(3);
    addGeometry(4);
    addGeometry(5);
    addGeometry(6);
    if (!_earlyAO) p.AddAmbientOcclusion(graph);
    const auto waterSurface = graph.AddPass(_water->Surface);
    graph.DependsOn(waterSurface, waterSimulation);
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

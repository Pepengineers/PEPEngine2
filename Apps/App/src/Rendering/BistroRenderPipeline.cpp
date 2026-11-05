#include "Rendering/BistroRenderPipeline.h"
#include <stdexcept>
#include "Common/GameTimer.h"
#include "Rendering/Grass/BistroGrassGeometry.h"
#include "Rendering/Water/BistroWaterRendering.h"
#include "Engine.RendererDX12/RenderPasses/GDX12GeometryViewPass.h"

namespace
{
    class GrassFramePreparationPass final : public GDX12RenderPass
    {
    public:
        explicit GrassFramePreparationPass(BistroGrassGeometry& grass) : _grass(grass) {}
        void RecordRenderGraph(GDX12RenderGraphBuilder& graph) override
        { graph.SetName("Bistro wind and grass roots"); graph.Read(SharedResource::Scene); graph.HasSideEffects(); }
        void Execute(GDX12CommandList* commands) override { _grass.PrepareGPU(commands); }
        std::vector<GDX12RenderGraph::BufferAccess> GetBufferAccesses() const override { return _grass.PreparationAccesses(); }
    private:
        BistroGrassGeometry& _grass;
    };
    class BistroGeometryPass final : public GDX12RenderPass
    {
    public:
        BistroGeometryPass(GDX12DeviceResources& resources, BistroGrassGeometry& grass, GDX12GeometryViewPass& pass)
            : _resources(resources), _grass(grass), _pass(pass) { _pass.SetSplitGeometry(true); }
        void RecordRenderGraph(GDX12RenderGraphBuilder& graph) override
        { _pass.RecordRenderGraph(graph); }
        void Execute(GDX12CommandList* commands) override
        {
            _pass.BeginGeometry(commands);
            for (UINT slot = 0; slot < _pass.GetGeometryViewCapacity(); ++slot)
                if (const auto* prepared = _pass.GetGeometryView(slot))
                {
                    _pass.ExecuteGeometryView(commands, slot);
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
            _pass.EndGeometry(commands);
        }
        std::vector<GDX12RenderGraph::BufferAccess> GetBufferAccesses() const override
        {
            auto accesses = _pass.GetBufferAccesses();
            for (UINT slot = 0; slot < _pass.GetGeometryViewCapacity(); ++slot)
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
        GDX12GeometryViewPass& _pass;
    };
}

BistroRenderPipeline::BistroRenderPipeline(std::shared_ptr<SceneManagerModule> scenes) : _scenes(std::move(scenes)) {}
BistroRenderPipeline::~BistroRenderPipeline() = default;

void BistroRenderPipeline::Initialize(RenderPipelineContext& context)
{
    if (_passes) throw std::logic_error("Render pipeline is already initialized");
    _common = &context.Common;
    _resources = &context.Primary;
    _earlyAO = GetAmbientOcclusionExecutionPoint() == 0;
    _grass = std::make_unique<BistroGrassGeometry>(_scenes, context.Runtime);
    context.Primary.GeometrySources.push_back(_grass.get());
    _water = std::make_unique<BistroWaterRendering>(_scenes);
    _passes = std::make_unique<CommonRenderPasses>(context.BackBuffer, GDX12WBOITTransparencyPass::DepthPartition::Near);
    _passes->Composition.SetConditionalTransparency(&_passes->Transparency);
    _passes->Composition.SetApplyOpaqueAerialPerspective(false);
    auto& passes = _passes->Bindings.PrimaryPasses;
    passes.insert(std::find(passes.begin(), passes.end(), &_passes->AO), &_water->Surface);
    passes.insert(std::find(passes.begin(), passes.end(), &_passes->Transparency),
        { &_water->Transparency, &_water->Composition, &_water->Resolve });
    _water->Transparency.SetFlag(RENDER_PASS_FLAG_USE_DOWNSCALED_RESOLUTION, true);
    _water->Composition.SetFlag(RENDER_PASS_FLAG_USE_DOWNSCALED_RESOLUTION, true);
    _passes->Setup(context);
    LinkInputs({ context.BackBuffer });
    _passes->Initialize();
    _geometryJobs.push_back(std::make_unique<GrassFramePreparationPass>(*_grass));
    for (auto* pass : GeometryPasses())
        _geometryJobs.push_back(std::make_unique<BistroGeometryPass>(*_resources, *_grass, *pass));
}

void BistroRenderPipeline::UpdateFrameSettings(RenderPipelineContext& context)
{
    if (_passes->Reconstruction.ApplySettings())
        context.Resize();
}

bool BistroRenderPipeline::PrepareFrame(RenderPipelineContext& context, GDX12CommandList* commands)
{
    const bool early = GetAmbientOcclusionExecutionPoint() == 0;
    bool rebuild = early != _earlyAO;
    _earlyAO = early;
    _grass->CollectScene(context.Common.GameTimer ? context.Common.GameTimer->DeltaTime() : 0);
    _grass->PrepareFrame(context.Primary, context.Common);
    _water->Surface.SetInstances(_water->CollectScene(context.Common.GameTimer ? context.Common.GameTimer->DeltaTime() : 0));
    _water->Surface.PrepareFrame();
    _water->Resolve.PrepareFrame();
    _passes->BindAmbientOcclusion(context.Common, _earlyAO);
    _passes->PrepareFrame(context, commands);
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
    p.ReflectionPreview.SetInputs({ p.Lighting.GetOutput(0), p.Opaque.GetOutput(2), p.Opaque.GetOutput(1),
        p.Opaque.GetOutput(3), p.Opaque.GetOutput(4) });
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
    LinkInputs(inputs);
    auto& p = *_passes;
    graph.AddPass(p.Environment);
    graph.AddPass(p.Culling);
    auto geometryTail = graph.AddPass(*_geometryJobs.at(0));
    const auto addGeometry = [&](UINT index)
    {
        const auto node = graph.AddPass(*_geometryJobs.at(index));
        graph.DependsOn(node, geometryTail);
        geometryTail = node;
    };
    addGeometry(1);
    graph.AddPass(p.ParticleSimulation);
    if (_earlyAO) graph.AddPass(p.AO);
    addGeometry(2);
    addGeometry(3);
    addGeometry(4);
    addGeometry(5);
    if (!_earlyAO) graph.AddPass(p.AO);
    graph.AddPass(_water->Surface);
    graph.AddPass(p.Lighting);
    graph.AddPass(p.ReflectionPreview);
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

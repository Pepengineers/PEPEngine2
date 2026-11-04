#include "Rendering/BistroRenderPipeline.h"
#include <stdexcept>
#include "Common/GameTimer.h"
#include "Rendering/Grass/BistroGrassGeometry.h"
#include "Engine.RendererDX12/RenderPasses/GDX12GeometryBatchPass.h"

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
    class GrassExpansionPass final : public GDX12RenderPass
    {
    public:
        GrassExpansionPass(BistroGrassGeometry& grass, GDX12GeometryViewPass& pass, UINT slot, UINT patch)
            : _grass(grass), _pass(pass), _slot(slot), _patch(patch) {}
        void RecordRenderGraph(GDX12RenderGraphBuilder& graph) override
        { graph.SetName("Bistro grass expansion"); graph.HasSideEffects(); }
        void Execute(GDX12CommandList* commands) override
        { if (const auto* view = _pass.GetGeometryView(_slot)) _grass.Expand(commands, *view, _patch); }
        std::vector<GDX12RenderGraph::BufferAccess> GetBufferAccesses() const override
        {
            const auto* view = _pass.GetGeometryView(_slot);
            return view ? _grass.ExpansionAccesses(*view, _patch) : std::vector<GDX12RenderGraph::BufferAccess>{};
        }
    private:
        BistroGrassGeometry& _grass;
        GDX12GeometryViewPass& _pass;
        UINT _slot, _patch;
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
    _passes = std::make_unique<CommonRenderPasses>(context.BackBuffer);
    _passes->Setup(context);
    LinkInputs({ context.BackBuffer });
    _passes->Initialize();
}

void BistroRenderPipeline::UpdateFrameSettings(RenderPipelineContext& context)
{
    if (_passes->Reconstruction.ApplySettings())
        context.Resize();
}

bool BistroRenderPipeline::PrepareFrame(RenderPipelineContext& context, GDX12CommandList* commands)
{
    _retiredGeometryJobs.clear();
    const bool early = GetAmbientOcclusionExecutionPoint() == 0;
    bool rebuild = early != _earlyAO;
    _earlyAO = early;
    _grass->CollectScene(context.Common.GameTimer ? context.Common.GameTimer->DeltaTime() : 0);
    _grass->PrepareFrame(context.Primary, context.Common);
    _passes->BindAmbientOcclusion(context.Common, _earlyAO);
    _passes->PrepareFrame(context, commands);
    const auto passes = GeometryPasses();
    rebuild |= _graphPatchCount != _grass->PatchCount() || _graphViewCapacities.size() != passes.size();
    for (UINT i = 0; i < passes.size(); ++i)
    {
        const auto capacity = passes[i]->GetGeometryViewCapacity();
        rebuild |= i >= _graphViewCapacities.size() || _graphViewCapacities[i] != capacity;
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
    p.Transparency.SetInputs({ p.Opaque.GetOutput(2), p.SunShadow.GetOutput(0), p.Environment.GetOutput(0),
        p.Environment.GetOutput(1), p.Environment.GetOutput(2), p.LocalShadow.GetOutput(0) });
    p.Particles.SetInputs({ p.Opaque.GetOutput(2), p.Transparency.GetOutput(0), p.Transparency.GetOutput(1) });
    p.Composition.SetInputs({ p.SSR.GetOutput(0), p.Transparency.GetOutput(0), p.Transparency.GetOutput(1),
        p.Opaque.GetOutput(2) });
    p.Composition.SetParticles(&p.Particles);
    p.DepthOfField.SetInputs({ p.Composition.GetOutput(0), p.Opaque.GetOutput(2) });
    p.MotionBlur.SetInputs({ p.DepthOfField.GetOutput(0), p.Opaque.GetOutput(2), p.Opaque.GetOutput(1) });
    p.Reconstruction.SetInputs({ p.Opaque.GetOutput(2), p.MotionBlur.GetOutput(1), p.MotionBlur.GetOutput(0) });
    p.Output.SetInputs({ p.Reconstruction.GetOutput(0), inputs.BackBuffer });
    p.FrameGeneration.SetInputs({ p.Opaque.GetOutput(2), p.MotionBlur.GetOutput(1), inputs.BackBuffer });
}

void BistroRenderPipeline::BuildGraph(GDX12RenderGraph& graph, const RenderPipelineGraphInputs& inputs)
{
    LinkInputs(inputs);
    auto& p = *_passes;
    graph.AddPass(p.Environment);
    graph.AddPass(p.Culling);
    // BuildGraph runs before the old graph is destroyed; retain its referenced jobs.
    _retiredGeometryJobs = std::move(_geometryJobs);
    _graphPatchCount = _grass->PatchCount();
    _graphViewCapacities.clear();
    for (auto* pass : GeometryPasses()) _graphViewCapacities.push_back(pass->GetGeometryViewCapacity());
    auto preparation = std::make_unique<GrassFramePreparationPass>(*_grass);
    auto geometryTail = graph.AddPass(*preparation);
    _geometryJobs.push_back(std::move(preparation));
    geometryTail = AddGeometry(graph, p.Depth, geometryTail);
    graph.AddPass(p.ParticleSimulation);
    if (_earlyAO) graph.AddPass(p.AO);
    geometryTail = AddGeometry(graph, p.SunShadow, geometryTail);
    geometryTail = AddGeometry(graph, p.LocalShadow, geometryTail);
    geometryTail = AddGeometry(graph, p.Reflections, geometryTail);
    AddGeometry(graph, p.Opaque, geometryTail);
    if (!_earlyAO) graph.AddPass(p.AO);
    graph.AddPass(p.Lighting);
    graph.AddPass(p.ReflectionPreview);
    graph.AddPass(p.SSR);
    graph.AddPass(p.Transparency);
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

GDX12RenderGraph::Pass BistroRenderPipeline::AddGeometry(GDX12RenderGraph& graph,
    GDX12GeometryViewPass& pass, GDX12RenderGraph::Pass prerequisite)
{
    pass.SetSplitGeometry(true);
    auto previous = graph.AddPass(pass);
    graph.DependsOn(previous, prerequisite);
    const auto append = [&](std::unique_ptr<GDX12RenderPass> job)
    {
        const auto node = graph.AddPass(*job);
        graph.DependsOn(node, previous);
        previous = node;
        _geometryJobs.push_back(std::move(job));
    };
    for (UINT slot = 0; slot < pass.GetGeometryViewCapacity(); ++slot)
    {
        append(std::make_unique<GDX12GeometryViewJobPass>(pass, slot));
        for (UINT patch = 0; patch < _graphPatchCount; ++patch)
        {
            append(std::make_unique<GrassExpansionPass>(*_grass, pass, slot, patch));
            append(std::make_unique<GDX12GeometryBatchPass>(*_resources, pass, slot, *_grass, patch, 1));
        }
    }
    append(std::make_unique<GDX12GeometryFinalizePass>(pass));
    return previous;
}

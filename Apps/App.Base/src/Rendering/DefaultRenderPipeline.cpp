#include "App.Base/Rendering/DefaultRenderPipeline.h"
#include <stdexcept>

void DefaultRenderPipeline::Initialize(RenderPipelineContext& context)
{
    if (_passes) throw std::logic_error("Render pipeline is already initialized");
    _common = &context.Common;
    _earlyAO = GetAmbientOcclusionExecutionPoint() == 0;
    _passes = std::make_unique<CommonRenderPasses>(context.BackBuffer);
    _passes->Setup(context);
    LinkInputs({ context.BackBuffer });
    _passes->Initialize();
    _passes->UpdateAmbientOcclusionRoute(context);
}

void DefaultRenderPipeline::UpdateFrameSettings(RenderPipelineContext& context)
{
    if (_passes->Reconstruction.ApplySettings())
        context.Resize();
}

bool DefaultRenderPipeline::PrepareFrame(RenderPipelineContext& context, GDX12CommandList* commands)
{
    const bool early = GetAmbientOcclusionExecutionPoint() == 0;
    bool rebuild = _passes->UpdateAmbientOcclusionRoute(context);
    rebuild |= early != _earlyAO;
    _earlyAO = early;
    _passes->BindAmbientOcclusion(context.Common, _earlyAO);
    _passes->PrepareFrame(context, commands);
    return rebuild;
}

void DefaultRenderPipeline::LinkInputs(const RenderPipelineGraphInputs& inputs)
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

void DefaultRenderPipeline::BuildGraph(GDX12RenderGraph& graph, const RenderPipelineGraphInputs& inputs)
{
    _passes->Bindings.SubmitLegacyReplacement = {};
    _passes->Bindings.RetirePlacedFrame = {};
    LinkInputs(inputs);
    auto& p = *_passes;
    graph.AddPass(p.Environment);
    graph.AddPass(p.Culling);
    graph.AddPass(p.Depth);
    graph.AddPass(p.ParticleSimulation);
    if (_earlyAO) p.AddAmbientOcclusion(graph, _earlyAO);
    graph.AddPass(p.SunShadow);
    graph.AddPass(p.LocalShadow);
    graph.AddPass(p.Reflections);
    graph.AddPass(p.Opaque);
    if (!_earlyAO) p.AddAmbientOcclusion(graph, _earlyAO);
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

void DefaultRenderPipeline::Resize(RenderPipelineContext& context)
{
    _passes->Resize(context);
}

const RenderPipelineBindings& DefaultRenderPipeline::GetBindings() const
{
    static const RenderPipelineBindings empty;
    return _passes ? _passes->Bindings : empty;
}

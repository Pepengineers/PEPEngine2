#pragma once

#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include <stdexcept>

class GDX12WBOITTransparencyPass;

enum class GDX12GeometryStage : std::uint8_t
{
    Depth, DepthNormals, GBuffer, Forward, Shadow, ReflectionCapture
};

struct GDX12CaptureLighting
{
    GDX12Texture* SunShadow = nullptr;
    GDX12Texture* Irradiance = nullptr;
    GDX12Texture* Prefiltered = nullptr;
    GDX12Texture* BRDF = nullptr;
    GDX12Texture* LocalShadows = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS ShadowConstants = 0;
};

struct GDX12GeometryContext
{
    GDX12GeometryStage Stage = GDX12GeometryStage::Forward;
    Matrix ViewProjection = Matrix::Identity;
    Vector3 Eye = Vector3::Zero;
    D3D12_GPU_VIRTUAL_ADDRESS CameraConstants = 0;
    const GDX12CaptureLighting* CaptureLighting = nullptr;
    bool StaticOnly = false;
    // Targets, viewport and scissor are already bound by the containing pass.
};

struct GDX12SceneContributions
{
    std::vector<GDX12LocalShadowCaster> Casters;
    uint64_t Revision = 14695981039346656037ull;
    bool HasBounds = false;
    BoundingBox Bounds;

    void Append(const BoundingBox& bounds, uint64_t revision, bool dynamic, bool castsShadows)
    {
        Revision = (Revision ^ revision) * 1099511628211ull;
        if (HasBounds) BoundingBox::CreateMerged(Bounds, Bounds, bounds);
        else Bounds = bounds;
        HasBounds = true;
        if (castsShadows) Casters.push_back({ bounds, revision, dynamic });
    }
};

struct GDX12TransparencyTail
{
    IRenderPassLink* Background = nullptr;
    IRenderPassLink* Accumulation = nullptr;
    IRenderPassLink* Revealage = nullptr;
    GDX12WBOITTransparencyPass* Producer = nullptr;
    bool ApplyOpaqueAerialPerspective = true;
};

class GDX12RenderExtensionBuildContext
{
public:
    using Stage = std::vector<GDX12RenderPass*>;

    GDX12RenderExtensionBuildContext(std::vector<std::unique_ptr<GDX12RenderPass>>& passes,
        GDX12DeviceResources* resources, GDX12DeviceResources* otherResources, RenderPipelineCommonData* common,
        IRenderPassLink* opaqueDepth = nullptr, IRenderPassLink* opaqueMotion = nullptr)
        : SceneDepth(opaqueDepth), SceneMotion(opaqueMotion), _passes(passes), _resources(resources),
          _otherResources(otherResources), _common(common), _opaqueDepth(opaqueDepth), _opaqueMotion(opaqueMotion) {}

    template<class T, class... Args> T* AddPass(Args&&... args)
    {
        auto pass = std::make_unique<T>(std::forward<Args>(args)...);
        auto* result = pass.get();
        result->Setup(_resources, _otherResources, _common);
        _passes.push_back(std::move(pass));
        return result;
    }

    IRenderPassLink* OpaqueDepth() const { return _opaqueDepth; }
    IRenderPassLink* OpaqueMotion() const { return _opaqueMotion; }

    IRenderPassLink* SceneColor = nullptr;
    IRenderPassLink* SceneDepth = nullptr;
    IRenderPassLink* SceneMotion = nullptr;
    IRenderPassLink* SunShadow = nullptr;
    IRenderPassLink* LocalShadows = nullptr;
    std::array<IRenderPassLink*, 4> Environment = {};

    void FinishStage()
    {
        if (_stageStart == _passes.size()) return;
        Stage stage;
        for (; _stageStart < _passes.size(); ++_stageStart) stage.push_back(_passes[_stageStart].get());
        Stages.push_back(std::move(stage));
    }
    void ApplyStageDependencies(GDX12RenderGraph& graph,
        const std::unordered_map<GDX12RenderPass*, GDX12RenderGraph::Pass>& ids) const
    {
        for (size_t stage = 1; stage < Stages.size(); ++stage)
            for (auto* pass : Stages[stage])
                for (auto* previous : Stages[stage - 1]) graph.DependsOn(ids.at(pass), ids.at(previous));
    }
    std::vector<Stage> Stages;

private:
    friend class RenderModule;
    void SetOpaqueInputs(IRenderPassLink* depth, IRenderPassLink* motion)
    {
        _opaqueDepth = SceneDepth = depth;
        _opaqueMotion = SceneMotion = motion;
    }
    std::vector<std::unique_ptr<GDX12RenderPass>>& _passes;
    GDX12DeviceResources* _resources;
    GDX12DeviceResources* _otherResources;
    RenderPipelineCommonData* _common;
    IRenderPassLink* _opaqueDepth = nullptr;
    IRenderPassLink* _opaqueMotion = nullptr;
    size_t _stageStart = 0;
};

class GDX12RenderExtension
{
public:
    virtual ~GDX12RenderExtension() = default;
    virtual void Initialize(GDX12DeviceResources&, RenderPipelineCommonData&) {}
    virtual void PrepareFrame(GDX12CommandList*, GDX12DeviceResources&, const RenderPipelineCommonData&) {}
    virtual void AppendSceneContributions(GDX12SceneContributions&) const {}
    virtual void DeclareGeometryResources(GDX12RenderGraphBuilder&) const {}
    virtual void DrawGeometry(GDX12CommandList*, const GDX12GeometryContext&) {}
    virtual void Resize(const RenderPipelineCommonData&) {}
    virtual void AfterDepthPrepass(GDX12RenderExtensionBuildContext&) {}
    virtual void AfterShadows(GDX12RenderExtensionBuildContext&) {}
    virtual void AfterOpaque(GDX12RenderExtensionBuildContext&) {}
    virtual void AfterLighting(GDX12RenderExtensionBuildContext&) {}
    virtual bool BuildTransparency(GDX12RenderExtensionBuildContext&, GDX12TransparencyTail&) { return false; }
    virtual void AfterTransparency(GDX12RenderExtensionBuildContext&) {}
    virtual void AfterPostProcess(GDX12RenderExtensionBuildContext&) {}
};

inline bool GDX12BuildExtensionTransparency(
    const std::vector<std::unique_ptr<GDX12RenderExtension>>& extensions,
    GDX12RenderExtensionBuildContext& context, GDX12TransparencyTail& tail)
{
    bool handled = false;
    for (auto& extension : extensions)
    {
        GDX12TransparencyTail candidate;
        if (!extension->BuildTransparency(context, candidate)) continue;
        if (handled) throw std::logic_error("Only one render extension may build the transparency segment");
        tail = candidate;
        handled = true;
    }
    return handled;
}

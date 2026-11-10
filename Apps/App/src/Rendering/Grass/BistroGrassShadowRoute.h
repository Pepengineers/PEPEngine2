#pragma once

#include "App.Base/Rendering/RenderShadowRoutes.h"
#include "Rendering/Grass/BistroGrassShadowReplica.h"
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"

class BistroGrassRoute;
struct RenderPipelineContext;
struct RenderPipelineBindings;

class BistroGrassShadowRoute final : public GDX12RenderPass
{
public:
    void PrepareFrame(RenderPipelineContext& context, BistroGrassGeometry& grass,
        const BistroGrassRoute& display, RenderShadowRoutes& shadows);
    void RecordRenderGraph(GDX12RenderGraphBuilder& graph) override;
    GDX12RenderGraph::Pass AddBoundary(GDX12RenderGraph& graph, GDX12RenderGraph::Pass prerequisite,
        RenderPipelineBindings& bindings);
    void CollectRetired() { if (_replica) _replica->CollectRetired(); }
    void FinishFrame();
    struct Diagnostics
    {
        GDX12Submission::TransferVersion Field;
        UINT64 Bytes = 0;
        UINT PreparedAtlases = 0, SubmittedAtlases = 0;
    };
    const Diagnostics& GetDiagnostics() const { return _diagnostics; }

private:
    using Replica = BistroGrassShadowReplica;
    struct Frame
    {
        const BistroGrassGeometry* Source;
        Replica::FrozenScene Scene;
        Replica::Field SourceField;
        Replica::CompletionPoint PrimaryReady;
        std::shared_ptr<const Replica::ImportedField> Imported;
        Vector3 SunEye;
    };
    RenderShadowRoutes::ApplicationFrame Resolve(Replica::Stamp stamp,
        const std::vector<GDX12ShadowOutput>& outputs, const GDX12ShadowSelection& selection);
    std::optional<Replica::CompletionPoint> Submit(const GDX12RenderGraph::LegacyBoundary& boundary,
        Replica::CompletionPoint prefix);

    RenderMGPURuntime* _runtime = nullptr;
    GDX12DeviceResources* _primary = nullptr;
    GDX12Device* _secondary = nullptr;
    std::shared_ptr<Replica> _replica;
    std::optional<Frame> _frame;
    Diagnostics _diagnostics;
};

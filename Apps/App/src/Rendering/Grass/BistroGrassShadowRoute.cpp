#include "Rendering/Grass/BistroGrassShadowRoute.h"
#include "Rendering/Grass/BistroGrassRoute.h"
#include "App.Base/Modules/RenderModule.h"
#include "Engine.RendererDX12/GDX12TextureResource.h"
#include <algorithm>
#include <utility>

namespace
{
using namespace GDX12Submission;
using Raster = CrossAdapterShadowRaster;
using Grass = BistroGrassShadowReplica;
using Graph = GDX12RenderGraph;
namespace Scene = CrossAdapterSceneReplica;

struct PendingReader
{
    std::shared_ptr<Grass> Replica;
    RenderMGPURuntime& Runtime;
    std::optional<Grass::Reader> Credit;
    bool Bound = false;

    PendingReader(std::shared_ptr<Grass> replica, RenderMGPURuntime& runtime)
        : Replica(std::move(replica)), Runtime(runtime) {}

    ~PendingReader()
    {
        if (Bound || !Credit || Runtime.IsStopped()) return;
        try { Replica->CancelReader(*Credit); }
        catch (...) { OutputDebugStringA("Grass shadow reader cancellation failed.\n"); }
    }

    void Bind(CompletionPoint point)
    {
        Replica->BindReader(Credit.value(), point);
        Bound = true;
    }
};

bool IsMaskedTexture(const Scene::Access& access)
{
    return access.Mode == Graph::Mode::Read && access.View.Native &&
        access.View.Native->D3DResource->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        (access.State & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) != 0;
}

void AppendStages(Raster::Input& input, std::vector<CrossAdapterShadows::RasterStage>& stages)
{
    for (auto& stage : stages) input.Preparation.push_back(std::move(stage));
}
}

void BistroGrassShadowRoute::PrepareFrame(RenderPipelineContext& context, BistroGrassGeometry& grass,
    const BistroGrassRoute& display, RenderShadowRoutes& shadows)
{
    if (_frame) throw std::logic_error("Previous grass shadow field was not closed");
    _diagnostics = {};
    const auto stamp = shadows.GetFrameStamp();
    auto scene = grass.CaptureShadowScene(context.Primary);
    if (!stamp.IsValid() || !scene || scene->Patches.empty()) return;

    Replica::Field field;
    field.Frame = stamp;
    if (const auto* selected = display.GetDisplay())
    {
        field.Manifest = selected->Fields.Current->GetLayout();
        field.Primary = { selected->CurrentView.Resource, selected->CurrentView.Identity };
        field.SourceVersion = selected->CurrentView.Version;
        field.Owner = selected->Fields.Current;
    }
    else
    {
        // While G tries its handoff it owns the canonical checkpoint identity.
        if (!display.GetDiagnostics().LocalAuthoritative || GetGrassMGPUControls().Enabled) return;
        const auto checkpoint = grass.PrepareLocalCheckpoint();
        if (!checkpoint) return;
        const auto view = shadows.IdentifyPrimaryView(checkpoint->Packed);
        field.Manifest = checkpoint->Layout;
        field.Primary = { view.Native, view.Identity };
        field.Owner = checkpoint;
    }

    _runtime = context.Runtime.GetMGPURuntime();
    _primary = &context.Primary;
    _secondary = context.Runtime.GetMGPUDevices().Secondary;
    if (!_runtime || !_secondary) return;
    if (!_replica) _replica = std::make_shared<Replica>(*_runtime, *_primary->Device, *_secondary);
    _frame.emplace(Frame{ &grass, {stamp, std::move(*scene)}, std::move(field), {}, {},
        context.Common.CameraView.Invert().Translation() });
    shadows.SetApplication([this](auto current, const auto& outputs, const auto& selection)
        { return Resolve(current, outputs, selection); });
}

void BistroGrassShadowRoute::RecordRenderGraph(GDX12RenderGraphBuilder& graph)
{
    graph.SetName("Bistro grass shadow field handoff");
    graph.Read(SharedResource::Scene);
    graph.HasSideEffects();
}

Graph::Pass BistroGrassShadowRoute::AddBoundary(Graph& graph, Graph::Pass prerequisite, RenderPipelineBindings& bindings)
{
    const auto node = graph.AddPass(*this);
    graph.DependsOn(node, prerequisite);
    graph.SetOptionalLegacySplit(*this, {}, {}, [this]() -> std::optional<std::vector<Graph::LegacyExternalView>>
    {
        if (!_frame) return std::nullopt;
        const auto& view = _frame->SourceField.Primary;
        return std::vector<Graph::LegacyExternalView>{{ view.Identity, view.Resource, Graph::Mode::Read,
            D3D12_RESOURCE_STATE_COPY_SOURCE, {} }};
    });
    bindings.LegacyReplacements.push_back({ this,
        [this](const auto& boundary, auto prefix) { return Submit(boundary, prefix); }, {}, {} });
    return node;
}

std::optional<CompletionPoint> BistroGrassShadowRoute::Submit(const Graph::LegacyBoundary& boundary, CompletionPoint prefix)
{
    auto& frame = _frame.value();
    if (boundary.ExternalViews.size() != 1 || boundary.ExternalViews.front().Native != frame.SourceField.Primary.Resource ||
        boundary.ExternalViews.front().Identity.Id != frame.SourceField.Primary.Identity.Id)
        throw std::logic_error("Grass shadow field lost its canonical legacy loan");
    frame.PrimaryReady = prefix;
    frame.Imported = _replica->SubmitField(frame.SourceField, prefix);
    if (!frame.Imported) return std::nullopt;
    _diagnostics.Field = frame.Imported->Version;
    _diagnostics.Bytes = UINT64(frame.Imported->Manifest.ElementCount) * sizeof(XMFLOAT4);
    frame.PrimaryReady = frame.Imported->PrimaryStaged;
    return frame.PrimaryReady;
}

RenderShadowRoutes::ApplicationFrame BistroGrassShadowRoute::Resolve(Replica::Stamp stamp,
    const std::vector<GDX12ShadowOutput>& outputs, const GDX12ShadowSelection& selection)
{
    RenderShadowRoutes::ApplicationFrame result;
    if (!_frame || !(_frame->Scene.Frame == stamp) || !_frame->PrimaryReady.IsValid()) return result;
    const auto& frame = *_frame;
    result.Source = frame.Source;
    for (const auto& patch : frame.Scene.Scene.Patches) result.Primitives.push_back(patch.Handle);
    std::vector<Replica::GroupRequest> requests;
    requests.reserve(outputs.size());
    for (UINT index = 0; index < outputs.size(); ++index)
    {
        const auto& output = outputs[index];
        Vector3 eye = frame.SunEye;
        if (output.Type != GDX12ShadowOutput::Kind::Sun)
        {
            const auto light = std::find_if(_primary->LocalLights.begin(), _primary->LocalLights.end(),
                [&](const auto& candidate) { return GetLocalShadowKey(candidate) == output.Light; });
            if (light == _primary->LocalLights.end()) return {};
            eye = Vector3(light->Position);
        }
        Replica::GroupRequest request{ output, index, eye, {} };
        std::vector<GDX12LocalShadowFrustum> frusta;
        for (UINT face = 0; face < output.ViewCount(); ++face) frusta.emplace_back(output.Views[face].ViewProj);
        for (const auto& patch : frame.Scene.Scene.Patches)
            if (std::any_of(frusta.begin(), frusta.end(), [&](const auto& frustum) { return frustum.Intersects(patch.Bounds); }))
                request.Patches.push_back(patch.Handle);
        requests.push_back(std::move(request));
    }

    const Replica::LocalField local{ stamp, frame.SourceField.Manifest, frame.SourceField.Primary,
        frame.SourceField.SourceVersion, frame.PrimaryReady, frame.SourceField.Owner };
    Replica::LocalField remote{};
    if (frame.Imported)
        remote = { stamp, frame.Imported->Manifest, frame.Imported->Secondary, frame.Imported->Version,
            frame.Imported->ImportReady, frame.Imported };
    auto primaryBatches = _replica->MakeBatches(*_primary->Device, frame.Scene, local, requests);
    auto secondaryBatches = _replica->MakeBatches(*_secondary, frame.Scene, remote, requests);
    for (size_t index = 0; index < requests.size(); ++index)
    {
        const auto& output = requests[index].Output;
        const bool selected = output.Type == GDX12ShadowOutput::Kind::Sun ? selection.IncludesSun() : selection.IncludesLocal(output.Light);
        if (primaryBatches[index])
            result.PrimaryBatches.insert(result.PrimaryBatches.end(), primaryBatches[index]->begin(), primaryBatches[index]->end());
        const auto& captured = selected && secondaryBatches[index] ? secondaryBatches[index] : primaryBatches[index];
        if (captured) result.CaptureBatches.insert(result.CaptureBatches.end(), captured->begin(), captured->end());
    }
    result.Prepare = [this, requests = std::move(requests), local, remote](const auto& capture, Raster::Input& primary,
        Raster::Input& secondary, const RenderShadowRoutes::LendView& lend)
    {
        const auto& frozen = _frame->Scene;
        const auto& active = *_primary->FrameConstants[_primary->CurrFrameConstantsIndex];
        const auto slots = active.MaterialCache->GetElementCount();
        Replica::MaterialBinding localMaterials{ primary.Replica->Materials, std::vector<UINT>(slots, UINT_MAX), {}, primary.Replica };
        Replica::MaterialBinding remoteMaterials{ secondary.Replica->Materials, std::vector<UINT>(slots, UINT_MAX), {}, secondary.Replica };
        for (size_t i = 0; i < capture.PrimaryMaterialSlots.size(); ++i)
        {
            const auto slot = capture.PrimaryMaterialSlots[i];
            localMaterials.ToLocal.at(slot) = slot;
            if (i < secondary.Replica->MaterialToLocal.size())
                remoteMaterials.ToLocal.at(slot) = secondary.Replica->MaterialToLocal[i];
        }

        const bool primaryGrass = std::any_of(primary.Replica->Groups.begin(), primary.Replica->Groups.end(),
            [](const auto& group) { return !group.Grass.empty(); });
        if (primaryGrass)
        {
            lend({ local.View.Resource, local.View.Identity, local.Version, local.Owner }, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            for (size_t i = 0; i < capture.Frame.Casters.size(); ++i)
            {
                const auto& caster = capture.Frame.Casters[i];
                const auto* mask = capture.Primary[i].Mask;
                if (caster.Kind != Scene::GeometryKind::ExpandedGrass || !mask || !mask->PrimaryDeviceTexture) continue;
                const auto view = lend({mask->PrimaryDeviceTexture->GetResourceOwner()}, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
                const auto duplicate = std::any_of(localMaterials.AlphaReads.begin(), localMaterials.AlphaReads.end(),
                    [&](const auto& read) { return read.View.Identity.Id == view.Identity.Id; });
                if (!duplicate)
                    localMaterials.AlphaReads.push_back({ {view.Native, view.Identity}, Graph::Mode::Read,
                        D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE });
            }
            auto prepared = _replica->Prepare(*_primary->Device, primary.MaterialHeap, frozen, local, requests, localMaterials);
            for (auto& group : primary.Groups)
                if (auto& views = prepared.Groups.at(group.ReplicaGroup)) group.Application = std::move(*views);
            AppendStages(primary, prepared.Preparation);
        }

        std::vector<Replica::GroupRequest> selected;
        for (const auto& group : secondary.Groups)
            if (secondary.Replica->Groups.at(group.ReplicaGroup).Reason == Scene::Rejection::None)
                selected.push_back(requests.at(group.ReplicaGroup));
        if (std::none_of(selected.begin(), selected.end(), [](const auto& group) { return !group.Patches.empty(); }))
            return;
        for (const auto& read : secondary.Replica->RasterAccesses)
            if (IsMaskedTexture(read))
                remoteMaterials.AlphaReads.push_back({ {read.View.Native, read.View.Identity}, read.Mode, read.State, read.View.Version });
        auto prepared = _replica->Prepare(*_secondary, secondary.MaterialHeap, frozen, remote, selected, remoteMaterials);
        bool needsReader = false;
        for (size_t index = 0; index < selected.size(); ++index)
        {
            auto found = std::find_if(secondary.Groups.begin(), secondary.Groups.end(),
                [&](const auto& group) { return group.ReplicaGroup == selected[index].ReplicaGroup; });
            if (found == secondary.Groups.end()) throw std::logic_error("Lost a selected Bistro grass shadow group");
            if (auto& views = prepared.Groups.at(index))
            {
                found->Application = std::move(*views);
                needsReader |= !selected[index].Patches.empty();
            }
        }
        if (!needsReader) return;
        auto lease = std::make_shared<PendingReader>(_replica, *_runtime);
        lease->Credit = _replica->AcquireReader(_frame->Imported);
        if (!lease->Credit)
        {
            for (auto& group : secondary.Groups) group.Application = {};
            return;
        }
        ++_diagnostics.PreparedAtlases;
        auto* queue = _secondary->GetCommandQueue();
        auto previous = std::move(secondary.CommitLegacy);
        secondary.CommitLegacy = [this, lease, queue, previous = std::move(previous)]
        {
            lease->Bind(queue->GetLastCompletionPoint());
            ++_diagnostics.SubmittedAtlases;
            if (previous) previous();
        };
        AppendStages(secondary, prepared.Preparation);
    };
    return result;
}

void BistroGrassShadowRoute::FinishFrame()
{
    if (_frame && _frame->Imported) _replica->FinishField();
    _frame.reset();
}

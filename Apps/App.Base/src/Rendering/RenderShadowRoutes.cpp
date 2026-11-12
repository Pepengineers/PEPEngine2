#include "App.Base/Rendering/RenderShadowRoutes.h"
#include "App.Base/Modules/RenderModule.h"
#include "Engine.RendererDX12/GDX12TextureResource.h"
#include "Engine.RendererDX12/RenderPasses/GDX12CascadedShadowPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12LocalShadowPass.h"
#include <algorithm>
#include <limits>
#include <utility>
#include <map>

namespace
{
using namespace GDX12Submission;
using Raster = CrossAdapterShadowRaster;
using Graph = GDX12RenderGraph;
namespace Replica = CrossAdapterSceneReplica;

size_t Index(GDX12ShadowAtlas atlas) { return static_cast<size_t>(atlas); }
}

struct RenderShadowRoutes::AtlasRoute
{
    struct Unit
    {
        Unit(GDX12Device& primary, GDX12Device& secondary, RenderMGPURuntime& runtime, GDX12ShadowAtlas atlas)
            : PrimaryRaster(primary, runtime, atlas), SecondaryRaster(secondary, runtime, atlas),
              Transfer(primary, secondary, runtime) {}
        Raster PrimaryRaster, SecondaryRaster;
        CrossAdapterShadows Transfer;
        GDX12ShadowUpdatePolicy Policy;
        std::optional<GDX12ShadowOutput> Displayed;
        double SourceTime = 0;
        ID3D12Resource* Atlas = nullptr;
    };
    AtlasRoute(GDX12Device& primary, GDX12Device& secondary, RenderMGPURuntime& runtime, GDX12ShadowAtlas atlas)
        : PrimaryRaster(primary, runtime, atlas), SecondaryRaster(secondary, runtime, atlas) {}

    Raster PrimaryRaster, SecondaryRaster;
    Raster::Input Primary, Secondary;
    std::shared_ptr<const Raster::SubmissionTrace> Trace;
    std::map<std::pair<UINT, uint64_t>, std::unique_ptr<Unit>> Units;
};

RenderShadowRoutes::RenderShadowRoutes(GDX12DeviceResources& primary, GDX12Device& secondary, RenderMGPURuntime& runtime,
    GDX12CascadedShadowPass& sun, GDX12LocalShadowPass& local)
    : _primary(primary), _secondary(secondary), _runtime(runtime), _sun(sun), _local(local), _transfer(*primary.Device, secondary, runtime),
      _replica(runtime, secondary), _quality(primary.ShadowQuality)
{
    for (auto atlas : { GDX12ShadowAtlas::Sun, GDX12ShadowAtlas::Local })
        _routes[Index(atlas)] = std::make_unique<AtlasRoute>(*primary.Device, secondary, runtime, atlas);
}

RenderShadowRoutes::~RenderShadowRoutes() = default;

void RenderShadowRoutes::UpdateQuality()
{
    const auto& next = _primary.ShadowQuality;
    if (next == _quality) return;
    const auto retire = [&](GDX12ShadowAtlas atlas)
    {
        _transfer.RetireAtlas(atlas);
        ClearUnits(atlas);
        auto& route = *_routes[Index(atlas)];
        route.Primary = {};
        route.Secondary = {};
        route.Trace.reset();
        route.PrimaryRaster.InvalidateCache();
        route.SecondaryRaster.InvalidateCache();
    };
    if (next.SunResolution != _quality.SunResolution) retire(GDX12ShadowAtlas::Sun);
    if (next.LocalResolution != _quality.LocalResolution) retire(GDX12ShadowAtlas::Local);
    _quality = next;
}

void RenderShadowRoutes::PrepareFrame(RenderModule& render)
{
    _replica.CollectRetired();
    _render = &render;
    if (_invocation == (std::numeric_limits<std::uint64_t>::max)())
        throw std::overflow_error("Shadow replica invocation ID exhausted");
    _stamp = { render.GetFrameSubmissionId(), ProducerInvocationId{++_invocation}, _primary.GeometryRevision };
    _sourceTime = ShadowSourceTimeSeconds();
    if (_policyEnabled != IsShadowPolicyEnabled())
    {
        ClearUnits(GDX12ShadowAtlas::Sun);
        ClearUnits(GDX12ShadowAtlas::Local);
        _policyEnabled = IsShadowPolicyEnabled();
        _policyFrame = 0;
    }
    ++_policyFrame;
    _diagnostics.clear();
    if (_policyEnabled)
    {
        if (_sun.DescribePreparedShadow({}))
        {
            const auto policy = ReadShadowUpdatePolicy();
            _diagnostics.push_back({ {}, true, policy.Secondary, false, policy.Latest, policy.Interval, _sun.GetDisplayedSourceTime() });
        }
        for (const auto& output : _local.DescribePreparedShadows({}))
        {
            const auto policy = ReadShadowUpdatePolicy(output.Light.EntityId);
            _diagnostics.push_back({ output.Light, false, policy.Secondary, false, policy.Latest, policy.Interval,
                _local.GetDisplayedSourceTime(output.Light) });
        }
    }
    _resolveApplication = {};
    for (const auto& route : _routes) route->Trace.reset();
    _selection = _transfer.ReadSelection(_local.DescribePreparedShadows({}));
}

void RenderShadowRoutes::Configure(Graph& graph, GDX12RenderPass& node, GDX12ShadowAtlas atlas,
    RenderPipelineBindings& bindings)
{
    const bool sun = atlas == GDX12ShadowAtlas::Sun;
    graph.SetOptionalLegacySplit(node, {}, { sun ? _sun.GetOutput(0) : _local.GetOutput(0) },
        [this, atlas] { return PrepareBoundary(atlas); });
    bindings.LegacyReplacements.push_back({ &node,
        [this, atlas](const auto& boundary, auto prefix) { return Submit(atlas, boundary, prefix); },
        {}, {} });
    if (sun)
        bindings.LegacyReplacements.push_back({ nullptr, {}, [this](auto tail) { RetireFrame(tail); }, {} });
}

void RenderShadowRoutes::RetireFrame(CompletionPoint tail)
{
    _transfer.RetireFrame(tail);
    for (const auto& route : _routes)
        for (const auto& [key, unit] : route->Units) unit->Transfer.RetireFrame(tail);
}

void RenderShadowRoutes::ClearUnits(GDX12ShadowAtlas atlas)
{
    auto& units = _routes[Index(atlas)]->Units;
    for (const auto& [key, unit] : units) unit->Transfer.RetireAtlas(atlas);
    units.clear();
}

void RenderShadowRoutes::Invalidate()
{
    _policyFrame = 0;
    _sun.ResetUpdateSchedule();
    _local.ResetUpdateSchedule();
    ClearUnits(GDX12ShadowAtlas::Sun);
    ClearUnits(GDX12ShadowAtlas::Local);
    for (const auto& route : _routes)
    {
        route->PrimaryRaster.InvalidateCache();
        route->Trace.reset();
    }
}

const Raster::SubmissionTrace* RenderShadowRoutes::GetTrace(GDX12ShadowAtlas atlas) const
{
    const auto* trace = _routes[Index(atlas)]->Trace.get();
    return trace && _render && trace->Stamp.Frame == _render->GetFrameSubmissionId() ? trace : nullptr;
}

Replica::ResourceView RenderShadowRoutes::IdentifyPrimaryView(std::shared_ptr<GDX12Resource> native)
{
    const auto identity = _runtime.IdentifyResource(native, *_primary.Device);
    return {std::move(native), identity, {}, {}};
}

std::optional<std::vector<Graph::LegacyExternalView>> RenderShadowRoutes::PrepareBoundary(GDX12ShadowAtlas atlas)
{
    auto& route = *_routes[Index(atlas)];
    route.Primary = {};
    route.Secondary = {};
    const auto fallback = [&]() -> std::optional<std::vector<Graph::LegacyExternalView>>
    {
        route.Primary = {};
        route.Secondary = {};
        route.PrimaryRaster.InvalidateCache();
        return std::nullopt;
    };
    const bool sun = atlas == GDX12ShadowAtlas::Sun;
    if (!_render || (sun ? !_selection.IncludesSun() : _selection.Scope == GDX12ShadowSelection::Mode::Sun))
        return fallback();

    std::vector<GDX12ShadowOutput> outputs, staticOutputs;
    if (sun)
    {
        if (auto output = _sun.DescribePreparedShadow({})) outputs.push_back(*output);
    }
    else
    {
        outputs = _local.DescribePreparedShadows({});
        staticOutputs = _local.DescribePreparedStaticShadows({});
    }
    if (outputs.empty()) return fallback();
    const auto selected = [&](const GDX12ShadowOutput& output)
        { return sun || _selection.IncludesLocal(output.Light); };
    if (std::none_of(outputs.begin(), outputs.end(), selected)) return fallback();

    const auto stamp = _stamp;
    auto application = _resolveApplication ? _resolveApplication(stamp, outputs, _selection) : ApplicationFrame{};
    auto capture = _render->CaptureShadowScene(stamp, outputs, application.CaptureBatches,
        application.Source, application.Primitives);
    auto primary = std::make_shared<Replica::PreparedFrame>();
    primary->Stamp = stamp;
    primary->Groups.resize(outputs.size());
    auto& frame = *_primary.FrameConstants[_primary.CurrFrameConstantsIndex];
    auto& a = route.Primary;
    auto& b = route.Secondary;
    a.Stamp = b.Stamp = stamp;
    b.Trace = std::make_shared<Raster::SubmissionTrace>();
    for (const auto& caster : capture.Frame.Casters)
        if (caster.MaterialIndex < capture.Frame.Materials.size() && capture.Frame.Materials[caster.MaterialIndex].Value.RenderLayer == 1)
            b.MaskedSources.push_back(caster.SourceId);
    a.MaterialHeap = _primary.SRV_UAV_Heap;
    a.PerFaceVisibility = b.PerFaceVisibility = _primary.LocalShadowCasters.has_value();
    a.CommitLegacy = [this, sun] { if (sun) _sun.InvalidateCache(); else _local.InvalidateCache(); };

    std::vector<Graph::LegacyExternalView> loans;
    const LendView lend = [&](Replica::ResourceView view, D3D12_RESOURCE_STATES state)
    {
        if (view.Identity.Id == ResourceId::Invalid)
            view = IdentifyPrimaryView(std::move(view.Native));
        const auto loan = std::find_if(loans.begin(), loans.end(), [&](const auto& item) { return item.Native == view.Native; });
        if (loan == loans.end())
        {
            loans.push_back({ view.Identity, view.Native, Graph::Mode::Read, state, {} });
        }
        else
        {
            const auto& previous = loan->Identity;
            const auto& current = view.Identity;
            if (previous.Id != current.Id || previous.Adapter != current.Adapter || previous.Allocation != current.Allocation ||
                previous.ResourceEpoch != current.ResourceEpoch || previous.Transfer != current.Transfer)
                throw std::logic_error("Shadow resource has competing primary identities");
            loan->PlacedState |= state;
        }
        return view;
    };

    const auto meshRead = [&](std::shared_ptr<GDX12Resource> native, D3D12_RESOURCE_STATES state)
    {
        auto view = lend({std::move(native)}, state);
        const auto existing = std::find_if(primary->RasterAccesses.begin(), primary->RasterAccesses.end(),
            [&](const auto& access) { return access.View.Native == view.Native; });
        if (existing == primary->RasterAccesses.end())
            primary->RasterAccesses.push_back({ view, view.Native->D3DResource, Graph::Mode::Read, state });
        else
            existing->State |= state;
        return view;
    };

    std::vector<GDX12IndirectDrawArgs> arguments;
    std::vector<UINT> drawIndices(capture.Frame.Casters.size(), UINT_MAX);
    for (size_t groupIndex = 0; groupIndex < outputs.size(); ++groupIndex)
    {
        Raster::Group group;
        group.Output = outputs[groupIndex];
        group.ReplicaGroup = static_cast<UINT>(groupIndex);
        if (!sun)
        {
            const auto found = std::find_if(staticOutputs.begin(), staticOutputs.end(), [&](const auto& output)
                { return output.Light == group.Output.Light; });
            if (found == staticOutputs.end()) return fallback();
            group.Static = *found;
        }
        a.Groups.push_back(group);
        if (selected(group.Output)) b.Groups.push_back(group);
        auto& recipe = primary->Groups[groupIndex];
        for (const auto& batch : application.PrimaryBatches)
            if (batch.GroupIndex == groupIndex) recipe.Grass.push_back(batch);
        for (const UINT index : capture.Frame.Groups[groupIndex])
        {
            if (index >= capture.Frame.Casters.size())
            {
                recipe.Reason = Replica::Rejection::MissingCaster;
                break;
            }
            const auto& caster = capture.Frame.Casters[index];
            if (caster.Kind == Replica::GeometryKind::ExpandedGrass)
            {
                if (std::none_of(recipe.Grass.begin(), recipe.Grass.end(), [&](const auto& batch)
                    { return batch.SourceId == caster.SourceId && batch.Stamp == stamp && batch.Device == _primary.Device; }))
                {
                    recipe.Reason = Replica::Rejection::MissingGeometry;
                    break;
                }
                continue;
            }
            const auto& source = capture.Primary[index];
            if (source.Instance == UINT_MAX)
            {
                recipe.Reason = Replica::Rejection::MissingGeometry;
                break;
            }
            if (drawIndices[index] == UINT_MAX)
            {
                const auto vertices = meshRead(_primary.GeometryBuffer->ShareVertexResource(),
                    D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
                const auto indices = meshRead(_primary.GeometryBuffer->ShareIndexResource(), D3D12_RESOURCE_STATE_INDEX_BUFFER);
                if (source.Mask && source.Mask->PrimaryDeviceTexture)
                    meshRead(source.Mask->PrimaryDeviceTexture->GetResourceOwner(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
                const auto offset = arguments.size() * sizeof(GDX12IndirectDrawArgs);
                const GDX12IndirectDrawArgs args{ source.Instance, source.IndexCount, 1, 0, 0, 0 };
                arguments.push_back(args);
                const auto vb = vertices.Native->D3DResource->GetGPUVirtualAddress();
                const auto ib = indices.Native->D3DResource->GetGPUVirtualAddress();
                drawIndices[index] = static_cast<UINT>(primary->Draws.size());
                primary->Draws.push_back({ caster.SourceId, caster.Kind, caster.ShadowFlags, caster.Dynamic,
                    *caster.Bounds, { vb + UINT64(source.VertexStart) * sizeof(Vertex), source.VertexCount * UINT(sizeof(Vertex)), sizeof(Vertex) },
                    { ib + UINT64(source.IndexStart) * sizeof(UINT), source.IndexCount * UINT(sizeof(UINT)), DXGI_FORMAT_R32_UINT },
                    args, offset });
            }
            recipe.Draws.push_back(drawIndices[index]);
        }
        if (!selected(group.Output)) capture.Frame.Groups[groupIndex].clear();
    }

    if (!arguments.empty() || !application.PrimaryBatches.empty())
    {
        const auto bind = [&](auto& buffer)
        {
            return Replica::Buffer{ meshRead(buffer->ShareResource(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
                buffer->GetSRV()->GPUHandle };
        };
        primary->Instances = bind(frame.ShadowInstances);
        primary->Transforms = bind(frame.ShadowTransforms);
        primary->Materials = bind(frame.ShadowMaterials);
        primary->AlphaTable = a.MaterialHeap->GetGPUHandle(Texture2D_StartIndex);
        if (!arguments.empty())
        {
            GDX12UploadBuffer<GDX12IndirectDrawArgs> indirect(_primary.Device, static_cast<UINT>(arguments.size()), EBufferType::Upload, false);
            indirect.CopyData(0, arguments.data(), static_cast<UINT>(arguments.size()));
            primary->Indirect.View = meshRead(indirect.ShareResource(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        }
    }

    auto remote = _replica.Prepare(capture.Frame, capture.Sources);
    if (!remote.Replica) return fallback();
    b.Replica = std::move(remote.Replica);
    b.Upload = std::move(remote.Upload);
    b.MaterialHeap = std::move(remote.MaterialHeap);
    b.CommitLegacy = std::move(remote.Commit);
    for (auto& stage : remote.Preparation)
    {
        CrossAdapterShadows::RasterStage prepared{ stage.Name, {}, std::move(stage.Record) };
        for (const auto& access : stage.Accesses)
            prepared.Accesses.push_back({ {access.View.Native, access.View.Identity}, access.Mode, access.State, access.View.Version });
        b.Preparation.push_back(std::move(prepared));
    }
    a.Replica = std::move(primary);
    if (application.Prepare) application.Prepare(capture, a, b, lend);
    if (IsShadowPolicyEnabled() && std::any_of(b.Groups.begin(), b.Groups.end(),
        [&](const auto& group) { return !route.SecondaryRaster.IsReady(b, group); })) return fallback();
    if (std::none_of(b.Groups.begin(), b.Groups.end(), [&](const auto& group) { return route.SecondaryRaster.IsReady(b, group); }))
        return fallback();
    return loans;
}

std::optional<CompletionPoint> RenderShadowRoutes::Submit(GDX12ShadowAtlas atlas,
    const Graph::LegacyBoundary& boundary, CompletionPoint prefix)
{
    auto& route = *_routes[Index(atlas)];
    if (boundary.Outputs.size() != 1) throw std::logic_error("Shadow route needs its canonical atlas");
    const auto target = IdentifyPrimaryView(boundary.Outputs.front().Native);
    route.Primary.Atlas = { target.Native, target.Identity };
    auto primary = std::exchange(route.Primary, {});
    auto secondary = std::exchange(route.Secondary, {});
    if (IsShadowPolicyEnabled()) return SubmitPolicy(atlas, std::move(primary), std::move(secondary), prefix);
    const auto trace = secondary.Trace;
    const auto receipt = SubmitPreparedShadows(_transfer, _selection, route.PrimaryRaster, std::move(primary),
        route.SecondaryRaster, std::move(secondary), prefix);
    if (receipt) route.Trace = trace;
    return receipt;
}


CompletionPoint RenderShadowRoutes::SubmitPolicy(GDX12ShadowAtlas atlas, Raster::Input primary,
    Raster::Input secondary, CompletionPoint prefix)
{
    auto& route = *_routes[Index(atlas)];
    const auto keyOf = [](const auto& output) { return std::pair{ output.Light.EntityId, output.Light.Generation }; };
    const auto require = [](bool condition, const char* message)
    { if (!condition) throw std::logic_error(message); };
    for (auto it = route.Units.begin(); it != route.Units.end();)
    {
        if (std::any_of(primary.Groups.begin(), primary.Groups.end(), [&](const auto& group)
            { return keyOf(group.Output) == it->first; })) { ++it; continue; }
        it->second->Transfer.RetireAtlas(atlas);
        it = route.Units.erase(it);
    }
    const auto originalCommit = primary.CommitLegacy;
    primary.CommitLegacy = [] {};
    const auto replicaCommit = secondary.CommitLegacy;
    auto replicaSubmitted = std::make_shared<bool>(false);
    secondary.CommitLegacy = [replicaCommit, replicaSubmitted]
    {
        if (*replicaSubmitted) return;
        if (replicaCommit) replicaCommit();
        *replicaSubmitted = true;
    };
    auto local = primary;
    local.Groups.clear();
    std::vector<AtlasRoute::Unit*> updatedPrimary;
    for (const auto& group : primary.Groups)
    {
        const auto policy = ReadShadowUpdatePolicy(group.Output.Light.EntityId);
        auto& owner = route.Units[keyOf(group.Output)];
        if (owner && (!(owner->Policy == policy) || owner->Atlas != primary.Atlas.Resource->D3DResource.Get() ||
            (owner->Displayed && !owner->Displayed->MatchesDestination(group.Output))))
        {
            owner->Transfer.RetireAtlas(atlas);
            owner.reset();
        }
        if (!owner)
        {
            owner = std::make_unique<AtlasRoute::Unit>(*_primary.Device, _secondary, _runtime, atlas);
            owner->Policy = policy;
            owner->Atlas = primary.Atlas.Resource->D3DResource.Get();
        }
        const bool due = ShadowUpdateDue(_policyFrame, policy.Interval, owner->Displayed.has_value());
        if (!policy.Secondary && due)
        {
            require(route.PrimaryRaster.IsReady(primary, group), "Primary shadow policy lacks a complete scene recipe");
            local.Groups.push_back(group);
            updatedPrimary.push_back(owner.get());
        }
    }
    if (!local.Groups.empty())
    {
        const auto rendered = local.Groups;
        const auto receipt = _transfer.SubmitPrimary(route.PrimaryRaster.Prepare(std::move(local)), prefix);
        require(receipt.has_value(), "Primary shadow policy was not submitted");
        prefix = *receipt;
        for (size_t i = 0; i < updatedPrimary.size(); ++i)
        {
            updatedPrimary[i]->Displayed = rendered[i].Output;
            updatedPrimary[i]->SourceTime = _sourceTime;
        }
    }
    auto trace = std::make_shared<Raster::SubmissionTrace>();
    trace->Stamp = _stamp;
    for (const auto& group : primary.Groups)
    {
        auto& unit = *route.Units.at(keyOf(group.Output));
        if (unit.Policy.Secondary)
        {
            auto a = primary;
            a.Groups = { group };
            auto b = secondary;
            const auto remote = std::find_if(secondary.Groups.begin(), secondary.Groups.end(), [&](const auto& candidate)
            { return keyOf(candidate.Output) == keyOf(group.Output); });
            require(remote != secondary.Groups.end() && unit.SecondaryRaster.IsReady(secondary, *remote),
                "Requested secondary shadow has no complete replica; refusing hidden primary fallback");
            b.Groups = { *remote };
            b.Trace = std::make_shared<Raster::SubmissionTrace>();
            if (*replicaSubmitted) { b.Preparation.clear(); b.Upload.reset(); }
            const auto evidence = b.Trace;
            GDX12ShadowSelection selection{ atlas == GDX12ShadowAtlas::Sun ?
                GDX12ShadowSelection::Mode::Sun : GDX12ShadowSelection::Mode::SpecificLight, group.Output.Light };
            const bool due = ShadowUpdateDue(_policyFrame, unit.Policy.Interval, unit.Displayed.has_value());
            const auto receipt = SubmitPreparedShadows(unit.Transfer, selection, unit.PrimaryRaster, std::move(a),
                unit.SecondaryRaster, std::move(b), prefix, unit.Policy.Latest, due, _sourceTime);
            require(receipt.has_value(), "Requested secondary shadow was not submitted; refusing hidden primary fallback");
            prefix = *receipt;
            const auto& displayed = unit.Transfer.GetDisplayedOutputs(atlas);
            require(displayed.size() == 1, "Shadow publication metadata is incomplete");
            unit.Displayed = displayed.front();
            unit.SourceTime = unit.Transfer.GetDisplayedSourceTime(atlas);
            trace->Groups.insert(trace->Groups.end(), evidence->Groups.begin(), evidence->Groups.end());
        }
        require(unit.Displayed.has_value(), "Shadow policy has no complete initial snapshot");
        if (!unit.Policy.Secondary && unit.Displayed->MatchesDepthInputs(group.Output))
            unit.SourceTime = atlas == GDX12ShadowAtlas::Sun ? _sun.GetDisplayedSourceTime() :
                _local.GetDisplayedSourceTime(group.Output.Light);
        auto& frame = *_primary.FrameConstants[_primary.CurrFrameConstantsIndex];
        if (atlas == GDX12ShadowAtlas::Sun)
            frame.DisplayShadowCB->CopyData(0, ShadowSnapshotConstants(*unit.Displayed));
        else
        {
            const auto light = std::find_if(_primary.LocalLights.begin(), _primary.LocalLights.end(), [&](const auto& current)
            { return GetLocalShadowKey(current) == unit.Displayed->Light; });
            require(light != _primary.LocalLights.end(), "Displayed shadow light was recycled");
            auto displayed = *light;
            require(ApplyShadowSnapshot(displayed, *unit.Displayed), "Displayed shadow metadata is incompatible");
            frame.DisplayShadowLights->CopyData(static_cast<UINT>(light - _primary.LocalLights.begin()), displayed);
        }
        const auto diagnostic = std::find_if(_diagnostics.begin(), _diagnostics.end(), [&](const auto& item)
        { return item.Sun == (atlas == GDX12ShadowAtlas::Sun) && item.Light == group.Output.Light; });
        if (diagnostic != _diagnostics.end())
        {
            diagnostic->EffectiveSecondary = unit.Policy.Secondary;
            diagnostic->SourceTime = unit.SourceTime;
            diagnostic->Transfer = unit.Transfer.GetStatistics(atlas);
        }
    }
    if (originalCommit) originalCommit();
    if (!trace->Groups.empty()) { trace->Finalize = prefix; route.Trace = trace; }
    return prefix;
}

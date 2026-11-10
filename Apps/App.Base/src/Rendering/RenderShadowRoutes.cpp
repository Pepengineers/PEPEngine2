#include "App.Base/Rendering/RenderShadowRoutes.h"
#include "App.Base/Modules/RenderModule.h"
#include "Engine.RendererDX12/GDX12TextureResource.h"
#include "Engine.RendererDX12/RenderPasses/GDX12CascadedShadowPass.h"
#include "Engine.RendererDX12/RenderPasses/GDX12LocalShadowPass.h"
#include <algorithm>
#include <limits>
#include <utility>

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
    AtlasRoute(GDX12Device& primary, GDX12Device& secondary, RenderMGPURuntime& runtime, GDX12ShadowAtlas atlas)
        : PrimaryRaster(primary, runtime, atlas), SecondaryRaster(secondary, runtime, atlas) {}

    Raster PrimaryRaster, SecondaryRaster;
    Raster::Input Primary, Secondary;
    std::shared_ptr<const Raster::SubmissionTrace> Trace;
};

RenderShadowRoutes::RenderShadowRoutes(GDX12DeviceResources& primary, GDX12Device& secondary, RenderMGPURuntime& runtime,
    GDX12CascadedShadowPass& sun, GDX12LocalShadowPass& local)
    : _primary(primary), _runtime(runtime), _sun(sun), _local(local), _transfer(*primary.Device, secondary, runtime),
      _replica(runtime, secondary)
{
    for (auto atlas : { GDX12ShadowAtlas::Sun, GDX12ShadowAtlas::Local })
        _routes[Index(atlas)] = std::make_unique<AtlasRoute>(*primary.Device, secondary, runtime, atlas);
}

RenderShadowRoutes::~RenderShadowRoutes() = default;

void RenderShadowRoutes::PrepareFrame(RenderModule& render)
{
    _replica.CollectRetired();
    _render = &render;
    if (_invocation == (std::numeric_limits<std::uint64_t>::max)())
        throw std::overflow_error("Shadow replica invocation ID exhausted");
    _stamp = { render.GetFrameSubmissionId(), ProducerInvocationId{++_invocation}, _primary.GeometryRevision };
    _resolveApplication = {};
    for (const auto& route : _routes) route->Trace.reset();
    _selection = _transfer.ReadSelection(_local.DescribePreparedShadows({}));
    _views.erase(std::remove_if(_views.begin(), _views.end(), [](const auto& entry) { return entry.Native.expired(); }), _views.end());
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

void RenderShadowRoutes::RetireFrame(CompletionPoint tail) { _transfer.RetireFrame(tail); }

void RenderShadowRoutes::Invalidate()
{
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
    const auto found = std::find_if(_views.begin(), _views.end(), [&](const auto& entry)
        { return entry.Native.lock() == native; });
    if (found != _views.end()) return { std::move(native), found->Identity, {}, {} };
    const ResourceIdentity identity{ _runtime.NewResourceId(), _primary.Device->GetSubmissionAdapterId(),
        _runtime.NewEpoch(), TransferId::Invalid, _runtime.NewAllocationId() };
    _views.push_back({ native, identity });
    return { std::move(native), identity, {}, {} };
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
    const auto trace = secondary.Trace;
    const auto receipt = SubmitPreparedShadows(_transfer, _selection, route.PrimaryRaster, std::move(primary),
        route.SecondaryRaster, std::move(secondary), prefix);
    if (receipt) route.Trace = trace;
    return receipt;
}

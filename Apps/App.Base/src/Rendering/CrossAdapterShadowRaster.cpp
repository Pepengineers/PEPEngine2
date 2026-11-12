#include "App.Base/Rendering/CrossAdapterShadowRaster.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12CommandQueue.h"
#include "Engine.RendererDX12/GDX12DescriptorHeap.h"
#include "Engine.RendererDX12/GDX12RootSignature.h"
#include "Engine.RendererDX12/GDX12ShaderCompiler.h"
#include <algorithm>
#include <iterator>
#include <map>
#include <stdexcept>
#include <unordered_map>

namespace
{
    using namespace GDX12Snapshot;
    using namespace GDX12Submission;
    namespace Replica = CrossAdapterSceneReplica;
    using Mode = GDX12RenderGraph::Mode;
    using Access = CrossAdapterShadows::Access;

    void Require(bool condition, const char* message)
    {
        if (!condition) throw std::invalid_argument(message);
    }

    auto GroupKey(const GDX12ShadowOutput& output)
    {
        return std::pair{ output.Light.EntityId, output.Light.Generation };
    }

    bool SameRegion(const GDX12ShadowRegion& a, const GDX12ShadowRegion& b)
    {
        return a.Slice == b.Slice && a.X == b.X && a.Y == b.Y && a.Width == b.Width && a.Height == b.Height;
    }

    bool SameProjection(const GDX12ShadowOutput& a, const GDX12ShadowOutput& b, UINT view)
    {
        return a.Type == b.Type && a.Light == b.Light && a.LightRevision == b.LightRevision &&
            SameRegion(a.Views[view].Region, b.Views[view].Region) &&
            std::memcmp(&a.Views[view].ViewProj, &b.Views[view].ViewProj, sizeof(XMFLOAT4X4)) == 0;
    }

    bool SameDepth(const GDX12ShadowOutput& a, const GDX12ShadowOutput& b, UINT view, bool perFace)
    {
        return SameProjection(a, b, view) && (perFace || a.SceneRevision == b.SceneRevision) &&
            a.Views[view].CasterRevision == b.Views[view].CasterRevision;
    }

    Access AsAccess(const Replica::Access& access)
    {
        Require(access.View.Native && access.View.Native->D3DResource.Get() == access.Handle.Get(),
            "Scene replica access no longer has its prepared native allocation");
        return { { access.View.Native, access.View.Identity }, access.Mode, access.State, access.View.Version };
    }

    struct RasterAccesses
    {
        void Append(Access access)
        {
            const auto [it, inserted] = ById.try_emplace(access.View.Identity.Id, Ordered.size());
            if (inserted)
            {
                Ordered.push_back(std::move(access));
                return;
            }
            auto& same = Ordered[it->second];
            Require(same.View.Resource == access.View.Resource && same.View.Identity.Allocation == access.View.Identity.Allocation &&
                same.View.Identity.Adapter == access.View.Identity.Adapter &&
                same.View.Identity.ResourceEpoch == access.View.Identity.ResourceEpoch &&
                same.View.Identity.Transfer == access.View.Identity.Transfer &&
                same.Mode == Mode::Read && access.Mode == Mode::Read && same.Version == access.Version,
                "Conflicting scene reads in a shadow raster stage");
            same.State |= access.State;
        }

        std::vector<Access> Ordered;
        std::unordered_map<ResourceId, size_t> ById;
    };
}

struct CrossAdapterShadowRaster::Pipeline
{
    Pipeline(GDX12Device& device, GDX12ShadowAtlas atlas)
    {
        GDX12RootSignatureDesc root;
        root.Constants.push_back(20);
        root.NumSingleSRVSlots = 3;
        root.SRVRanges.emplace_back(Texture2D_RangeLength);
        root.StaticSamplers = GetStaticSamplers();
        Root = std::make_unique<GDX12RootSignature>(&device, root);
        const D3D_SHADER_MACRO defines[] = { { "SHADOW_REPLICA_IA", "1" }, { nullptr, nullptr } };
        auto& compiler = GDX12ShaderCompiler::GetInstance();
        const auto vs = compiler.CompileShader(&device, SHADERS_FOLDER "ShadowPass.hlsl", defines, "VS", "vs");
        const auto ps = compiler.CompileShader(&device, SHADERS_FOLDER "ShadowPass.hlsl", defines, "PS", "ps");
        const D3D12_INPUT_ELEMENT_DESC layout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Engine::Core::Vertex, Position),
                D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(Engine::Core::Vertex, TexCoord),
                D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
        };
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = Root->GetRootSignature().Get();
        desc.InputLayout = { layout, static_cast<UINT>(std::size(layout)) };
        desc.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
        desc.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
        desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        desc.RasterizerState.FrontCounterClockwise = TRUE;
        desc.RasterizerState.DepthBias = atlas == GDX12ShadowAtlas::Sun ? 500 : 250;
        desc.RasterizerState.SlopeScaledDepthBias = 1.5f;
        desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        desc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        desc.SampleMask = UINT_MAX;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        desc.SampleDesc.Count = 1;
        ThrowIfFailed(device.GetDevice()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&PSO)));
    }

    std::unique_ptr<GDX12RootSignature> Root;
    ComPtr<ID3D12PipelineState> PSO;
};

struct CrossAdapterShadowRaster::Cache
{
    struct Entry { GDX12ShadowOutput Output; std::optional<GDX12ShadowOutput> Static; };
    PhysicalView Atlas, StaticAtlas;
    GDX12ResourceHandle AtlasHandle;
    bool PerFaceVisibility = false;
    std::map<std::pair<UINT, uint64_t>, Entry> Groups;
};

struct CrossAdapterShadowRaster::Work
{
    struct View
    {
        UINT Group, Face;
        std::vector<UINT> Draws;
        D3D12_CPU_DESCRIPTOR_HANDLE DSV{};
    };

    void Raster(GDX12CommandList& commands, const std::vector<View>& views, bool clear, bool application) const
    {
        auto* list = commands.GetCommandList().Get();
        const bool sun = Data.Groups[views.front().Group].Output.Type == GDX12ShadowOutput::Kind::Sun;
        if (clear && sun)
            list->ClearDepthStencilView(views.front().DSV, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        for (const auto& view : views)
        {
            const auto& group = Data.Groups[view.Group];
            const auto& projection = group.Output.Views[view.Face];
            const auto& r = projection.Region;
            const D3D12_RECT rect{ static_cast<LONG>(r.X), static_cast<LONG>(r.Y),
                static_cast<LONG>(r.X + r.Width), static_cast<LONG>(r.Y + r.Height) };
            if (clear && !sun) list->ClearDepthStencilView(view.DSV, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
            list->OMSetRenderTargets(0, nullptr, FALSE, &view.DSV);
            commands.SetViewport({ static_cast<float>(r.X), static_cast<float>(r.Y),
                static_cast<float>(r.Width), static_cast<float>(r.Height), 0, 1 });
            commands.SetScissorRect(rect);
            if (!view.Draws.empty())
            {
                const auto& replica = *Data.Replica;
                commands.SetDescriptorHeaps({ Data.MaterialHeap.get() });
                commands.SetGraphicsRootSignature(Mesh->Root.get());
                commands.SetPipelineState(Mesh->PSO);
                commands.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                commands.SetGraphicsSRV(0, replica.Materials.SRV);
                commands.SetGraphicsSRV(1, replica.Transforms.SRV);
                commands.SetGraphicsSRV(2, replica.Instances.SRV);
                commands.SetGraphicsSRV(3, replica.AlphaTable);
                const UINT drawConstants[4] = {};
                list->SetGraphicsRoot32BitConstants(0, 4, drawConstants, 0);
                list->SetGraphicsRoot32BitConstants(0, 16, &projection.ViewProj, 4);
                commands.InvalidateGeometryBuffer();
                for (const UINT index : view.Draws)
                {
                    const auto& draw = replica.Draws[index];
                    list->IASetVertexBuffers(0, 1, &draw.Vertices);
                    list->IASetIndexBuffer(&draw.Indices);
                    const auto& args = draw.Arguments;
                    list->SetGraphicsRoot32BitConstant(0, args.InstanceID, 0);
                    commands.DrawIndexedInstanced(args.IndexCountPerInstance, args.InstanceCount,
                        args.StartIndexLocation, args.BaseVertexLocation, args.StartInstanceLocation);
                    if (Data.Trace)
                    {
                        auto& evidence = Data.Trace->Groups[view.Group];
                        ++evidence.MeshDraws;
                        evidence.SkinnedDraws += draw.Kind == Replica::GeometryKind::Skinned;
                        evidence.MaskedDraws += std::binary_search(Data.MaskedSources.begin(), Data.MaskedSources.end(), draw.SourceId);
                    }
                }
            }
            if (application && group.Application[view.Face])
            {
                const auto& app = *group.Application[view.Face];
                app.Draw(commands);
                if (Data.Trace && app.RecordedSources)
                {
                    auto& recorded = Data.Trace->Groups[view.Group].RecordedGrass;
                    for (const auto source : *app.RecordedSources)
                        if (std::find(app.Sources.begin(), app.Sources.end(), source) != app.Sources.end() &&
                            std::find(recorded.begin(), recorded.end(), source) == recorded.end())
                            recorded.push_back(source);
                }
            }
        }
    }

    Input Data;
    std::shared_ptr<Pipeline> Mesh;
    std::shared_ptr<Cache> Next;
    std::unique_ptr<GDX12DescriptorHeap> DSVs;
    std::vector<View> Static, Final;
};

CrossAdapterShadowRaster::CrossAdapterShadowRaster(GDX12Device& device, RenderMGPURuntime& runtime, GDX12ShadowAtlas atlas)
    : _device(device), _runtime(runtime), _atlas(atlas) {}

CrossAdapterShadowRaster::~CrossAdapterShadowRaster() = default;

PhysicalView CrossAdapterShadowRaster::CreateDepth(UINT localLayers, UINT resolution) const
{
    const bool sun = _atlas == GDX12ShadowAtlas::Sun;
    if (resolution == 0) resolution = sun ? SunShadowCascadeResolution : LocalShadowResolution;
    Require(sun ? IsSunShadowResolution(resolution) : IsLocalShadowResolution(resolution),
        "Invalid shadow raster resolution");
    const UINT extent = sun ? 2 * resolution : resolution;
    const UINT layers = sun ? 1 : localLayers;
    Require(layers > 0 && layers <= D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION, "Invalid shadow raster atlas slice count");
    const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32_TYPELESS, extent, extent,
        static_cast<UINT16>(layers), 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    D3D12_CLEAR_VALUE clear = {};
    clear.Format = DXGI_FORMAT_D32_FLOAT;
    clear.DepthStencil.Depth = 1.0f;
    auto resource = std::make_shared<GDX12Resource>();
    resource->D3DResource = _device.CreateResource(D3D12_HEAP_TYPE_DEFAULT, desc, D3D12_RESOURCE_STATE_COMMON, &clear);
    return { std::move(resource), { _runtime.NewResourceId(), _device.GetSubmissionAdapterId(), _runtime.NewEpoch(),
        TransferId::Invalid, _runtime.NewAllocationId() } };
}

bool CrossAdapterShadowRaster::IsReady(const Input& input, const Group& group) const
{
    if (!input.Replica || !input.Stamp.IsValid() || !(input.Replica->Stamp == input.Stamp) ||
        group.ReplicaGroup >= input.Replica->Groups.size()) return false;
    const auto& recipe = input.Replica->Groups[group.ReplicaGroup];
    if (recipe.Reason != Replica::Rejection::None) return false;
    std::vector<uint64_t> grassSources;
    for (const auto& grass : recipe.Grass)
    {
        if (!(grass.Stamp == input.Stamp) || grass.Device != &_device || !grass.Owner) return false;
        grassSources.push_back(grass.SourceId);
    }
    std::sort(grassSources.begin(), grassSources.end());
    for (UINT face = 0; face < group.Output.ViewCount(); ++face)
    {
        const auto& app = group.Application[face];
        if (!app)
        {
            if (!grassSources.empty()) return false;
            continue;
        }
        auto sources = app->Sources;
        std::sort(sources.begin(), sources.end());
        if (!(app->Stamp == input.Stamp) || sources != grassSources || !app->Draw || app->Owners.empty()) return false;
    }
    return true;
}

CrossAdapterShadows::RasterPlan CrossAdapterShadowRaster::Prepare(Input input)
{
    const bool sun = _atlas == GDX12ShadowAtlas::Sun;
    Require(input.Atlas.Resource && input.Atlas.Resource->D3DResource &&
        input.Atlas.Identity.Adapter == _device.GetSubmissionAdapterId(), "Shadow raster needs a retained device-local atlas");
    const auto atlasDesc = input.Atlas.Resource->D3DResource->GetDesc();
    const UINT resolution = sun ? atlasDesc.Height / 2 : atlasDesc.Height;
    const UINT extent = sun ? 2 * resolution : resolution;
    Require(atlasDesc.Format == DXGI_FORMAT_R32_TYPELESS && atlasDesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        atlasDesc.Width == extent && atlasDesc.Height == extent && atlasDesc.MipLevels == 1 && atlasDesc.SampleDesc.Count == 1 &&
        (sun ? IsSunShadowResolution(resolution) : IsLocalShadowResolution(resolution)) &&
        (atlasDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) && (!sun || atlasDesc.DepthOrArraySize == 1),
        "Invalid native shadow raster atlas");
    std::vector<GDX12ShadowOutput> outputs;
    for (const auto& group : input.Groups) outputs.push_back(group.Output);
    if (!outputs.empty())
    {
        const auto layout = GDX12ShadowTransferLayout::Build(_atlas, std::move(outputs));
        Require(layout && layout->Extent() == resolution, "Invalid shadow raster group mapping");
    }
    auto work = std::make_shared<Work>();
    work->Data = std::move(input);
    work->Next = std::make_shared<Cache>();
    auto& data = work->Data;
    if (data.Trace) std::sort(data.MaskedSources.begin(), data.MaskedSources.end());
    auto& next = *work->Next;
    next.Atlas = data.Atlas;
    next.AtlasHandle = data.Atlas.Resource->D3DResource;
    next.PerFaceVisibility = data.PerFaceVisibility;
    const bool sameAtlas = _cache && _cache->Atlas.Identity.Id == next.Atlas.Identity.Id &&
        _cache->AtlasHandle.Get() == next.AtlasHandle.Get();
    if (!sun && !data.Groups.empty())
        next.StaticAtlas = sameAtlas && _cache->StaticAtlas.Resource ? _cache->StaticAtlas :
            CreateDepth(atlasDesc.DepthOrArraySize, resolution);

    for (UINT index = 0; index < data.Groups.size(); ++index)
    {
        const auto& group = data.Groups[index];
        Require(IsReady(data, group), "Missing or stale shadow draw recipe");
        if (!sun)
            Require(group.Static && group.Static->StaticOnly, "Local shadow raster requires matching static metadata");
        const auto& output = group.Output;
        const auto& recipe = data.Replica->Groups[group.ReplicaGroup];
        if (data.Trace)
        {
            auto& evidence = data.Trace->Groups.at(index);
            evidence.AdmittedGrass.reserve(recipe.Grass.size());
            evidence.RecordedGrass.reserve(recipe.Grass.size());
            for (const auto& grass : recipe.Grass) evidence.AdmittedGrass.push_back(grass.SourceId);
        }
        const Cache::Entry* old = nullptr;
        if (sameAtlas && _cache->PerFaceVisibility == data.PerFaceVisibility)
        {
            const auto match = _cache->Groups.find(GroupKey(output));
            if (match != _cache->Groups.end()) old = &match->second;
        }
        next.Groups.emplace(GroupKey(output), Cache::Entry{ output, group.Static });
        const bool sunDirty = sun && (!old || output.EveryFrame || !output.MatchesDepthInputs(old->Output));
        for (UINT face = 0; face < output.ViewCount(); ++face)
        {
            Require(output.Views[face].Region.Slice < atlasDesc.DepthOrArraySize,
                "Shadow raster view exceeds its atlas slices");
            if (!sun)
                Require(SameProjection(output, *group.Static, face), "Static and final local shadow layouts differ");
            const bool staticDirty = !sun && (!old || !old->Static ||
                !SameDepth(*group.Static, *old->Static, face, data.PerFaceVisibility));
            const bool dirty = sun ? sunDirty : (staticDirty || !old || output.EveryFrame ||
                !SameDepth(output, old->Output, face, data.PerFaceVisibility));
            if (!dirty) continue;
            Work::View dynamic{ index, face, {} };
            Work::View stationary{ index, face, {} };
            const bool emptyDynamic = !sun && data.PerFaceVisibility &&
                output.Views[face].CasterRevision == group.Static->Views[face].CasterRevision;
            const GDX12LocalShadowFrustum frustum(output.Views[face].ViewProj);
            for (UINT drawIndex : recipe.Draws)
            {
                const auto& draw = data.Replica->Draws.at(drawIndex);
                if ((emptyDynamic && draw.Dynamic) || !frustum.Intersects(draw.Bounds)) continue;
                if (sun || draw.Dynamic) dynamic.Draws.push_back(drawIndex);
                else if (staticDirty) stationary.Draws.push_back(drawIndex);
            }
            if (staticDirty) work->Static.push_back(std::move(stationary));
            work->Final.push_back(std::move(dynamic));
        }
    }

    CrossAdapterShadows::RasterPlan plan;
    plan.ExactInputs = data.ExactInputs;
    plan.Stages = std::move(data.Preparation);
    plan.Owners.push_back(work);
    plan.Commit = [this, previous = _cache, work]
    {
        if (_cache != previous) throw std::logic_error("Stale shadow cache commit");
        _cache = work->Next;
        if (work->Data.CommitLegacy) work->Data.CommitLegacy();
    };
    if (work->Final.empty()) return plan;
    if (!_pipeline) _pipeline = std::make_shared<Pipeline>(_device, _atlas);
    work->Mesh = _pipeline;
    work->DSVs = std::make_unique<GDX12DescriptorHeap>(&_device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV,
        static_cast<UINT>(work->Static.size() + work->Final.size()), D3D12_DESCRIPTOR_HEAP_FLAG_NONE);
    UINT descriptor = 0;
    const auto createViews = [&](std::vector<Work::View>& views, const PhysicalView& atlas)
    {
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
        dsv.Format = DXGI_FORMAT_D32_FLOAT;
        dsv.ViewDimension = sun ? D3D12_DSV_DIMENSION_TEXTURE2D : D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        for (auto& view : views)
        {
            if (!sun)
            {
                dsv.Texture2DArray.ArraySize = 1;
                dsv.Texture2DArray.FirstArraySlice = data.Groups[view.Group].Output.Views[view.Face].Region.Slice;
            }
            view.DSV = work->DSVs->GetCPUHandle(descriptor++);
            _device.GetDevice()->CreateDepthStencilView(atlas.Resource->D3DResource.Get(), &dsv, view.DSV);
        }
    };
    createViews(work->Static, next.StaticAtlas);
    createViews(work->Final, next.Atlas);

    const auto meshAccesses = [&](const std::vector<Work::View>& views)
    {
        RasterAccesses accesses;
        if (std::none_of(views.begin(), views.end(), [](const auto& view) { return !view.Draws.empty(); })) return accesses;
        Require(data.MaterialHeap != nullptr, "Replica mesh draw has no material binding");
        accesses.Ordered.reserve(data.Replica->RasterAccesses.size());
        accesses.ById.reserve(data.Replica->RasterAccesses.size());
        for (const auto& access : data.Replica->RasterAccesses)
        {
            if (data.Replica->Indirect.View.Native && access.View.Identity.Id == data.Replica->Indirect.View.Identity.Id)
                continue;
            auto use = AsAccess(access);
            if (use.State & D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE)
                use.State |= D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
            accesses.Append(std::move(use));
        }
        return accesses;
    };
    if (data.Upload && !data.Upload->Get().UploadAccesses.empty())
    {
        Require(&data.Upload->Get() == data.Replica.get(), "Shadow upload does not belong to the raster recipe");
        std::vector<Access> accesses;
        for (const auto& access : data.Replica->UploadAccesses) accesses.push_back(AsAccess(access));
        plan.Stages.push_back({ "Shadow scene upload", std::move(accesses),
            [work](GDX12CommandList& commands) { Replica::RecordUploads(commands, *work->Data.Upload); } });
    }
    if (!work->Static.empty())
    {
        auto accesses = meshAccesses(work->Static);
        accesses.Append({ next.StaticAtlas, Mode::Write, D3D12_RESOURCE_STATE_DEPTH_WRITE });
        plan.Stages.push_back({ "Shadow static depth", std::move(accesses.Ordered),
            [work](GDX12CommandList& commands) { work->Raster(commands, work->Static, true, false); } });
    }
    if (!sun)
    {
        plan.Stages.push_back({ "Shadow static depth copy", {
            { next.StaticAtlas, Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE },
            { next.Atlas, Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST } },
            [work](GDX12CommandList& commands)
            {
                auto* list = commands.GetCommandList().Get();
                for (const auto& view : work->Final)
                {
                    const UINT slice = work->Data.Groups[view.Group].Output.Views[view.Face].Region.Slice;
                    const CD3DX12_TEXTURE_COPY_LOCATION source(work->Next->StaticAtlas.Resource->D3DResource.Get(), slice);
                    const CD3DX12_TEXTURE_COPY_LOCATION dest(work->Next->Atlas.Resource->D3DResource.Get(), slice);
                    list->CopyTextureRegion(&dest, 0, 0, 0, &source, nullptr);
                }
            } });
    }
    auto accesses = meshAccesses(work->Final);
    accesses.Append({ next.Atlas, Mode::Write, D3D12_RESOURCE_STATE_DEPTH_WRITE });
    for (const auto& view : work->Final)
    {
        const auto& group = data.Groups[view.Group];
        const auto& app = group.Application[view.Face];
        if (!app) continue;
        for (const auto& access : app->Accesses) accesses.Append(access);
        for (const auto& grass : data.Replica->Groups[group.ReplicaGroup].Grass)
        {
            bool versionedReady = false;
            for (const auto& read : grass.Reads)
            {
                auto access = AsAccess(read);
                if (!grass.SamePlan && access.Version.IsValid())
                {
                    versionedReady = true;
                    const auto same = std::find_if(plan.ExactInputs.begin(), plan.ExactInputs.end(), [&](const auto& input)
                        { return input.View.Identity.Id == access.View.Identity.Id && input.Version == access.Version; });
                    if (same == plan.ExactInputs.end())
                        plan.ExactInputs.push_back({ access.View, access.Version, grass.Ready, grass.Owner });
                    else Require(same->Ready.Timeline() == grass.Ready.Timeline() && same->Ready.Value() == grass.Ready.Value(),
                        "Conflicting grass readiness for one version");
                }
                accesses.Append(std::move(access));
            }
            if (!grass.SamePlan && !versionedReady)
            {
                const auto* direct = _device.GetCommandQueue();
                Require(grass.Ready.IsValid() && grass.Ready.Timeline() == direct->GetTimelineIdentity() &&
                    grass.Ready.Value() <= direct->GetLastCompletionPoint().Value(),
                    "Unversioned grass input must have issued on the raster DIRECT lane");
            }
        }
    }
    plan.Stages.push_back({ "Shadow final depth", std::move(accesses.Ordered),
        [work, sun](GDX12CommandList& commands) { work->Raster(commands, work->Final, sun, true); } });
    return plan;
}

void CrossAdapterShadowRaster::InvalidateCache()
{
    _cache.reset();
}

PhysicalView CrossAdapterShadowRaster::ReuseDepth(UINT localLayers, UINT resolution) const
{
    const bool sun = _atlas == GDX12ShadowAtlas::Sun;
    if (resolution == 0) resolution = sun ? SunShadowCascadeResolution : LocalShadowResolution;
    if (_cache && _cache->AtlasHandle.Get() == _cache->Atlas.Resource->D3DResource.Get())
    {
        const auto desc = _cache->AtlasHandle->GetDesc();
        if (desc.Width == (sun ? 2 * resolution : resolution) && (sun || desc.DepthOrArraySize == localLayers))
            return _cache->Atlas;
    }
    return CreateDepth(localLayers, resolution);
}

std::optional<CompletionPoint> SubmitPreparedShadows(CrossAdapterShadows& transfer, const GDX12ShadowSelection& selection,
    CrossAdapterShadowRaster& primaryRaster, CrossAdapterShadowRaster::Input primary,
    CrossAdapterShadowRaster& secondaryRaster, CrossAdapterShadowRaster::Input secondary, CompletionPoint prefix,
    bool latest, bool update, double sourceTime)
{
    const auto fallback = [&]() -> std::optional<CompletionPoint>
    {
        primaryRaster.InvalidateCache();
        return std::nullopt;
    };
    if (!IsMultiGPUShadowsEnabled() || !primary.CommitLegacy || primaryRaster.Atlas() != secondaryRaster.Atlas() ||
        primary.Stamp.Frame != secondary.Stamp.Frame || primary.Stamp.SceneRevision != secondary.Stamp.SceneRevision)
        return fallback();
    std::vector<GDX12ShadowOutput> outputs;
    std::vector<bool> ready;
    std::vector<size_t> matches;
    for (const auto& group : primary.Groups)
    {
        outputs.push_back(group.Output);
        const auto match = std::find_if(secondary.Groups.begin(), secondary.Groups.end(), [&](const auto& candidate)
        {
            if (!group.Output.MatchesDepthInputs(candidate.Output)) return false;
            return primaryRaster.Atlas() == GDX12ShadowAtlas::Sun ||
                (group.Static && candidate.Static && group.Static->MatchesDepthInputs(*candidate.Static));
        });
        matches.push_back(static_cast<size_t>(match - secondary.Groups.begin()));
        ready.push_back(match != secondary.Groups.end() && secondaryRaster.IsReady(secondary, *match));
    }
    auto selected = CrossAdapterShadows::Select(primaryRaster.Atlas(), selection, outputs, ready);
    if (!selected) return fallback();

    std::vector<CrossAdapterShadowRaster::Group> local, remote;
    for (const auto index : selected->PrimaryGroups)
    {
        if (!primaryRaster.IsReady(primary, primary.Groups[index])) return fallback();
        local.push_back(std::move(primary.Groups[index]));
    }
    for (size_t i = 0; i < selected->SecondaryGroups.size(); ++i)
    {
        auto group = std::move(secondary.Groups[matches[selected->SecondaryGroups[i]]]);
        group.Output = selected->SecondaryOutput[i];
        if (group.Static)
            for (UINT face = 0; face < group.Output.ViewCount(); ++face)
                group.Static->Views[face].Region = group.Output.Views[face].Region;
        remote.push_back(std::move(group));
    }
    primary.Groups = std::move(local);
    secondary.Groups = std::move(remote);
    const auto trace = secondary.Trace;
    if (trace)
    {
        trace->Stamp = secondary.Stamp;
        trace->Groups.resize(secondary.Groups.size());
        for (size_t index = 0; index < secondary.Groups.size(); ++index)
        {
            const auto& compact = secondary.Groups[index].Output;
            const auto& original = outputs[selected->SecondaryGroups[index]];
            auto& evidence = trace->Groups[index];
            evidence.Kind = original.Type;
            evidence.Light = original.Light;
            evidence.ViewCount = original.ViewCount();
            for (UINT face = 0; face < evidence.ViewCount; ++face)
            {
                evidence.CompactViews[face] = compact.Views[face].Region;
                evidence.PrimaryViews[face] = original.Views[face].Region;
            }
        }
    }
    const auto primaryAtlas = primary.Atlas;
    PhysicalView secondaryAtlas;
    CrossAdapterShadows::RasterPlan localPlan, remotePlan;
    try
    {
        secondary.Atlas = secondaryRaster.ReuseDepth(selected->Layout.Layers(), selected->Layout.Extent());
        secondaryAtlas = secondary.Atlas;
        localPlan = primaryRaster.Prepare(std::move(primary));
        remotePlan = secondaryRaster.Prepare(std::move(secondary));
    }
    catch (const DxException& error) { OutputDebugStringW(error.ToString().c_str()); return fallback(); }
    catch (const std::exception& error) { OutputDebugStringA(error.what()); return fallback(); }

    auto submitted = transfer.Submit(*selected, outputs, primaryAtlas, secondaryAtlas, localPlan, remotePlan, prefix,
        latest, update, sourceTime);
    if (trace && submitted)
    {
        trace->Finalize = *submitted;
        trace->SecondaryDepth = secondaryAtlas.Resource;
    }
    return submitted ? submitted : fallback();
}

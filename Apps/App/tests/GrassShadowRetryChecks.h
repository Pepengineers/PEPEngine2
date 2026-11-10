#pragma once

#ifdef _DEBUG

#include "Rendering/Grass/BistroGrassShadowReplica.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12CommandQueue.h"
#include "Engine.RendererDX12/GDX12Device.h"
#include "Engine.RendererDX12/GDX12UploadBuffer.h"
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace GrassShadowRetryChecks
{
    using namespace GDX12Submission;
    using Graph = GDX12RenderGraph;
    using Replica = BistroGrassShadowReplica;
    using Access = CrossAdapterShadows::Access;
    using GDX12Snapshot::PhysicalView;

    inline void RequireRetry(bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }

    class PlacedPass final : public GDX12RenderPass
    {
    public:
        PlacedPass(const char* name, AdapterId adapter, std::function<void(GDX12CommandList&)> record)
            : _name(name), _adapter(adapter), _record(std::move(record)) {}

        std::vector<std::pair<Graph::Resource, Access>> Uses;

        bool SupportsGraphQueue(QueueKind kind) const override { return kind == QueueKind::Direct; }
        void RecordRenderGraph(GDX12RenderGraphBuilder& graph) override
        {
            graph.SetName(_name);
            graph.SetPlacement({ _adapter, QueueKind::Direct });
            graph.DeclareCompleteAccesses();
            graph.HasSideEffects();
            for (const auto& [view, access] : Uses) graph.Use(view, access.Mode, access.State);
        }
        void RecordPlacedCommands(GDX12CommandList* list, const Graph::BoundResources& bound) override
        {
            for (const auto& [view, access] : Uses)
                RequireRetry(bound.Get(view) == access.View.Resource->D3DResource.Get(), "G2 retry graph bound the wrong resource");
            _record(*list);
        }

    private:
        const char* _name;
        AdapterId _adapter;
        std::function<void(GDX12CommandList&)> _record;
    };

    struct QueueGate
    {
        ComPtr<ID3D12Fence> Fence;
        ~QueueGate() { if (Fence) Fence->Signal(1); }
    };
}

inline void CheckNativeGrassShadowRetry(GDX12Device& primary, GDX12Device& secondary, RenderMGPURuntime& runtime)
{
    using namespace GrassShadowRetryChecks;
    using Mode = Graph::Mode;
    namespace Scene = CrossAdapterSceneReplica;
    const auto adapter = secondary.GetSubmissionAdapterId();
    RequireRetry(primary.GetSubmissionAdapterId() != adapter, "G2 retry requires two adapters");
    auto& queue = *secondary.GetCommandQueue();
    const auto identity = [&]
    {
        return ResourceIdentity{ runtime.NewResourceId(), adapter, runtime.NewEpoch(), TransferId::Invalid,
            runtime.NewAllocationId() };
    };
    const auto makeView = [&](const D3D12_RESOURCE_DESC& desc, const D3D12_CLEAR_VALUE* clear = nullptr)
    {
        auto native = std::make_shared<GDX12Resource>();
        native->D3DResource = secondary.CreateResource(D3D12_HEAP_TYPE_DEFAULT, desc, D3D12_RESOURCE_STATE_COMMON, clear);
        return PhysicalView{ std::move(native), identity() };
    };

    auto heap = std::make_shared<GDX12DescriptorHeap>(&secondary, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        Texture2D_StartIndex + Texture2D_RangeLength, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
    Scene::AlphaTexture mask;
    mask.Texture = Engine::Core::TextureHandle{ 1, 1 };
    mask.ContentRevision = 1;
    mask.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    mask.Width = mask.Height = 2;
    mask.MipLevels = 1;
    mask.Mips.push_back({ std::vector<std::byte>(16, std::byte{255}), 8 });
    mask.Mips.front().Bytes[7] = mask.Mips.front().Bytes[15] = std::byte{0};
    auto stagedMask = Scene::StageAlpha(secondary, *heap, mask, identity);
    RequireRetry(stagedMask && stagedMask->Upload, "G2 retry could not stage its masked material texture");
    auto alpha = std::make_shared<Scene::StagedAlpha>(std::move(*stagedMask));
    const auto& alphaView = alpha->Texture.SecondaryView;
    auto materials = std::make_shared<GDX12UploadBuffer<GDX12MaterialConstants>>(&secondary, 1, EBufferType::Upload, false);
    materials->CreateSRV(heap.get(), heap->GetAvailableIndex(ConstantsResources));
    GDX12MaterialConstants masked;
    masked.RenderLayer = 1;
    masked.DiffuseIndex = alpha->Texture.SecondarySRVIndex - Texture2D_StartIndex;
    materials->CopyData(0, masked);
    const auto materialView = std::shared_ptr<GDX12Resource>(materials, &materials->GetResource());
    const Replica::MaterialBinding binding{
        { { materialView, identity() }, materials->GetSRV()->GPUHandle }, { 0 },
        { { { alphaView.Native, alphaView.Identity }, Mode::Read, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE } }, alpha };

    Replica::Layout layout;
    layout.InteractionArea = XMFLOAT4(80, 0, 100, 100);
    const UINT cells = layout.InteractionGrid * layout.InteractionGrid;
    layout.Presence = { 0, cells };
    layout.Trail = { cells, cells };
    layout.ElementCount = 2 * cells;
    const UINT64 packedBytes = UINT64(layout.ElementCount) * sizeof(XMFLOAT4);
    const auto field = makeView(CD3DX12_RESOURCE_DESC::Buffer(packedBytes));
    GDX12UploadBuffer<XMFLOAT4> zeroField(&secondary, layout.ElementCount, EBufferType::Upload, false);
    auto* setup = queue.GetCommandList();
    setup->ResourceBarrier({ field.Resource->GetCopyDestBarrier(), alphaView.Native->GetCopyDestBarrier() });
    setup->GetCommandList()->CopyBufferRegion(field.Resource->D3DResource.Get(), 0,
        zeroField.GetResource().D3DResource.Get(), 0, packedBytes);
    Scene::RecordAlphaUpload(*setup, *alpha->Upload);
    setup->ResourceBarrier({ field.Resource->GetCommonBarrier(), alphaView.Native->GetCommonBarrier() });
    queue.ExecuteCommandList(setup);
    const auto ready = queue.GetLastCompletionPoint();
    RequireRetry(ready.IsValid(), "G2 retry setup has no issued point");
    queue.CPUWaitForFenceValue(ready.Value());

    const Replica::Stamp stamp{ FrameSubmissionId{1}, ProducerInvocationId{1}, 1 };
    const Replica::LocalField local{ stamp, layout, field, {}, ready, field.Resource };
    Replica::FrozenScene capture{ stamp, {} };
    capture.Scene.Controls.WindEnabled = false;
    GDX12GrassInstance patch;
    patch.Id = 1;
    patch.Implementation = GDX12GrassImplementation::CrossedRibbons;
    patch.RootsCm = {{ 76, 0, 0 }, { 84, 0, 0 }};
    patch.HeightVariation = 0;
    patch.WidthCm = 14;
    const GDX12RenderPrimitiveHandle source{ 17, 1 };
    capture.Scene.Patches.push_back({ source, patch, GrassLocalBounds(patch) });

    constexpr UINT extent = 96;
    const Vector3 eye(80, 20, 120);
    const auto view = Matrix::CreateLookAt(eye, Vector3(80, 20, 0), Vector3::Up);
    const auto projection = Matrix::CreateOrthographic(72, 72, 1, 250);
    GDX12ShadowOutput output;
    output.Type = GDX12ShadowOutput::Kind::Spot;
    output.Views.front().Region = { 0, 0, 0, extent, extent };
    output.Views.front().ViewProj = (view * projection).Transpose();
    std::vector<Replica::GroupRequest> groups{ { output, 0, eye, { source } } };

    D3D12_CLEAR_VALUE clear{};
    clear.Format = DXGI_FORMAT_D32_FLOAT;
    clear.DepthStencil.Depth = 1.0f;
    const auto depthDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32_TYPELESS, extent, extent, 1, 1, 1, 0,
        D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    const auto depth = makeView(depthDesc, &clear);
    GDX12DescriptorHeap dsvHeap(&secondary, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE);
    const auto dsv = dsvHeap.GetCPUHandle();
    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
    dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    secondary.GetDevice()->CreateDepthStencilView(depth.Resource->D3DResource.Get(), &dsvDesc, dsv);

    Replica replica(runtime, primary, secondary);
    Graph::PlanExecutor executor(1);
    const auto submit = [&](GDX12Device& device, const char* name, std::vector<Access> accesses,
        std::vector<std::shared_ptr<const void>> owners, std::function<void(GDX12CommandList&)> record)
    {
        Graph graph;
        Graph::ExecutionBindings bindings;
        bindings.Queues.push_back({ &device, device.GetCommandQueue() });
        bindings.KeepAlive = std::move(owners);
        PlacedPass pass(name, device.GetSubmissionAdapterId(), std::move(record));
        for (const auto& access : accesses)
        {
            const auto id = graph.Import("G2 retry resource");
            graph.RegisterResource(id, access.View.Identity);
            bindings.Views.push_back({ id, access.View.Identity, access.View.Resource });
            pass.Uses.emplace_back(id, access);
        }
        graph.AddPass(pass);
        graph.CompilePlan();
        return executor.TrySubmit(graph, bindings);
    };

    UINT rootRecordings = 0, drawRecordings = 0;
    const auto render = [&](const Replica::PreparedAtlas& prepared)
    {
        RequireRetry(prepared.Groups.size() == 1 && prepared.Groups.front(), "G2 retry did not prepare the complete spot");
        const auto& app = prepared.Groups.front()->front();
        auto accesses = app ? app->Accesses : std::vector<Access>{};
        accesses.push_back({ depth, Mode::Write, D3D12_RESOURCE_STATE_DEPTH_WRITE });
        auto owners = app ? app->Owners : std::vector<std::shared_ptr<const void>>{};
        return submit(secondary, "G2 point-root shadow retry", std::move(accesses), std::move(owners),
            [&](GDX12CommandList& commands)
            {
                for (const auto& stage : prepared.Preparation)
                {
                    RequireRetry(stage.Accesses.empty(), "G2 retry fixture expects private root upload resources");
                    stage.Record(commands);
                    ++rootRecordings;
                }
                commands.GetCommandList()->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
                commands.GetCommandList()->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
                commands.SetViewport({ 0, 0, static_cast<float>(extent), static_cast<float>(extent), 0, 1 });
                commands.SetScissorRect({ 0, 0, extent, extent });
                if (app)
                {
                    app->Draw(commands);
                    ++drawRecordings;
                }
            });
    };

    CompletionPoint blocker;
    std::weak_ptr<const void> refusedSlot;
    {
        QueueGate hold;
        auto& primaryQueue = *primary.GetCommandQueue();
        ThrowIfFailed(primary.GetDevice()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&hold.Fence)));
        ThrowIfFailed(primaryQueue.GetCommandQueue()->Wait(hold.Fence.Get(), 1));
        const auto pending = submit(primary, "G2 retry capacity blocker", {}, {}, [](GDX12CommandList&) {});
        RequireRetry(pending && pending->Batches.size() == 1, "G2 retry could not occupy the executor");
        blocker = pending->Batches.front();
        RequireRetry(!blocker.IsReachedBy(primaryQueue.GetCompletionProgress()), "G2 retry capacity blocker already completed");

        const auto rejected = replica.Prepare(secondary, heap, capture, local, groups, binding);
        refusedSlot = rejected.Roots.Slot;
        const std::vector<std::uint64_t> sourceIds{ source.Index };
        RequireRetry(!refusedSlot.expired() && rejected.Roots.PointSources == sourceIds &&
            rejected.Preparation.size() == 1, "G2 retry did not prepare the point-source root upload");
        const auto before = queue.GetLastCompletionPoint();
        RequireRetry(!render(rejected) && rootRecordings == 0 && drawRecordings == 0 &&
            queue.GetLastCompletionPoint().Value() == before.Value(), "G2 retry did not refuse before recording");
    }
    primary.GetCommandQueue()->CPUWaitForFenceValue(blocker.Value());
    executor.CollectCompleted();
    replica.CollectRetired();

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 readbackBytes = 0;
    secondary.GetDevice()->GetCopyableFootprints(&depthDesc, 0, 1, 0, &footprint, nullptr, nullptr, &readbackBytes);
    auto readback = secondary.CreateResource(D3D12_HEAP_TYPE_READBACK, CD3DX12_RESOURCE_DESC::Buffer(readbackBytes),
        D3D12_RESOURCE_STATE_COPY_DEST);
    const auto readDepth = [&]
    {
        auto* list = queue.GetCommandList();
        list->ResourceBarrier({ depth.Resource->GetCopySourceBarrier() });
        const CD3DX12_TEXTURE_COPY_LOCATION sourceCopy(depth.Resource->D3DResource.Get(), 0);
        const CD3DX12_TEXTURE_COPY_LOCATION destination(readback.Get(), footprint);
        list->GetCommandList()->CopyTextureRegion(&destination, 0, 0, 0, &sourceCopy, nullptr);
        list->ResourceBarrier({ depth.Resource->GetCommonBarrier() });
        queue.ExecuteCommandList(list);
        queue.CPUWaitForFenceValue(queue.GetLastCompletionPoint().Value());
        executor.CollectCompleted();

        std::vector<float> pixels(extent * extent);
        void* mapped = nullptr;
        const D3D12_RANGE read{ 0, static_cast<SIZE_T>(readbackBytes) }, noWrite{ 0, 0 };
        ThrowIfFailed(readback->Map(0, &read, &mapped));
        for (UINT y = 0; y < extent; ++y)
        {
            const auto* row = reinterpret_cast<const float*>(static_cast<const std::byte*>(mapped) +
                footprint.Offset + y * footprint.Footprint.RowPitch);
            std::copy_n(row, extent, pixels.data() + y * extent);
        }
        readback->Unmap(0, &noWrite);
        return pixels;
    };

    std::vector<float> grassDepth;
    {
        const auto retry = replica.Prepare(secondary, heap, capture, local, groups, binding);
        const auto& slot = retry.Roots.Slot;
        RequireRetry(!slot.expired() && !slot.owner_before(refusedSlot) && !refusedSlot.owner_before(slot) &&
            retry.Roots.PointSources == std::vector<std::uint64_t>{ source.Index },
            "G2 retry did not reuse the declined native slot and point source");
        const auto accepted = render(retry);
        const auto& app = retry.Groups.front()->front();
        const auto sources = app ? app->RecordedSources : nullptr;
        RequireRetry(accepted && accepted->Batches.size() == 1 && rootRecordings == 1 && drawRecordings == 1 &&
            sources && *sources == retry.Roots.PointSources, "G2 retry did not issue its root upload and native draw");
        grassDepth = readDepth();
    }
    groups.front().Patches.clear();
    const auto control = replica.Prepare(secondary, heap, capture, local, groups, binding);
    const auto cleared = render(control);
    RequireRetry(cleared && cleared->Batches.size() == 1 && rootRecordings == 1 && drawRecordings == 1,
        "G2 retry did not submit the no-grass control");
    const auto emptyDepth = readDepth();
    bool nearer = false;
    for (size_t i = 0; i < grassDepth.size(); ++i)
    {
        RequireRetry(emptyDepth[i] == 1.0f && std::isfinite(grassDepth[i]), "G2 retry depth control or sample is invalid");
        nearer |= grassDepth[i] >= 0.0f && grassDepth[i] < emptyDepth[i] - 1.0e-4f;
    }
    RequireRetry(nearer, "Retried masked point roots produced no depth inside their off-origin view");
    replica.CollectRetired();
}

#endif

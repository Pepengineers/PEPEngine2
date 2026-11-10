#include "App.Base/Rendering/CrossAdapterUI.h"
#include "App.Base/Rendering/RenderMGPURuntime.h"
#include "Common/ConsoleVariables.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12CommandQueue.h"
#include "Engine.RendererDX12/GDX12SnapshotTransport.h"
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include "Engine.UI/UIScreenComposite.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

using namespace GDX12Submission;

namespace Engine::UI
{
    namespace
    {
        using Graph = GDX12RenderGraph;
        using Mode = Graph::Mode;
        using GDX12Snapshot::PhysicalView;
        using GDX12Snapshot::Transport;
        using GDX12Snapshot::Result;

        AutoConsoleVariable<bool> Enabled(L"Render.UI.Offload", false, L"Use the secondary adapter for selected UI surfaces.");
        AutoConsoleVariable<UIOffloadScope> Scope(L"Render.UI.OffloadScope", UIOffloadScope::Whole, L"Whole UI or one world canvas.");
        AutoConsoleVariable<std::uint64_t> Selected(L"Render.UI.CanvasId", 0, L"Stable canvas ID for Canvas scope.");
        AutoConsoleVariable<UIVisualFreshness> Freshness(L"Render.UI.Freshness", UIVisualFreshness::Exact,
            L"Exact or latest completed snapshot for world canvases. Screen always uses Exact.");
        bool ValidUpdateHz(double value) { return std::isfinite(value) && (value == 0 || (value >= 1 && value <= 240)); }
        bool ValidMaxAgeMs(double value) { return std::isfinite(value) && value >= 0 && value <= 60000; }
        AutoConsoleVariable<double> UpdateHz(L"Render.UI.UpdateHz", 0,
            L"Secondary world canvas producer limit. 0 uncapped, 1 to 240 Hz.", EConsoleVariableFlags::Default, ValidUpdateHz);
        AutoConsoleVariable<double> MaxAgeMs(L"Render.UI.MaxAgeMs", 0,
            L"Latest UI age diagnostic threshold. 0 disabled, up to 60000 ms.", EConsoleVariableFlags::Default, ValidMaxAgeMs);

        void Require(bool condition, const char* reason)
        {
            if (!condition) throw std::logic_error(reason);
        }

        struct RasterBusy {};

        class Batch final : public GDX12RenderPass
        {
        public:
            using Recorder = std::function<void(GDX12CommandList&, const Graph::BoundResources&)>;
            Batch(const char* name, Placement lane, std::vector<Graph::Access> accesses, Recorder recorder)
                : Name(name), Lane(lane), Accesses(std::move(accesses)), Record(std::move(recorder)) {}

            void RecordRenderGraph(GDX12RenderGraphBuilder& builder) override
            {
                builder.SetName(Name);
                builder.SetPlacement(Lane);
                builder.DeclareCompleteAccesses();
                builder.HasSideEffects();
                for (const auto& access : Accesses)
                    if (access.Version.IsValid()) builder.UseVersion(access.Id, access.Usage, access.Version, access.State);
                    else builder.Use(access.Id, access.Usage, access.State);
            }

            bool SupportsGraphQueue(QueueKind kind) const override { return kind == Lane.Queue; }
            void RecordPlacedCommands(GDX12CommandList* list, const Graph::BoundResources& views) override
            { Record(*list, views); }

        private:
            const char* Name;
            Placement Lane;
            std::vector<Graph::Access> Accesses;
            Recorder Record;
        };

        struct Plan
        {
            Graph::Resource Bind(const char* name, const PhysicalView& view, bool imported = false, bool shared = false)
            {
                const auto id = imported ? RenderGraph.Import(name) : RenderGraph.Create(name);
                RenderGraph.RegisterResource(id, view.Identity, shared);
                Bindings.Views.push_back({id, view.Identity, view.Resource});
                return id;
            }

            Graph::Pass Add(const char* name, Placement lane, std::vector<Graph::Access> accesses, Batch::Recorder record)
            {
                Passes.push_back(std::make_unique<Batch>(name, lane, std::move(accesses), std::move(record)));
                return RenderGraph.AddPass(*Passes.back());
            }

            void Import(Graph::Resource id, TransferVersion version, CompletionPoint ready,
                std::shared_ptr<const void> reservation, GDX12CommandQueue* issuer = nullptr,
                ResourceIdentity sharedSource = {})
            {
                RenderGraph.ImportVersion(id, version);
                Bindings.ExactInputs.push_back({id, version, ready, std::move(reservation), issuer, sharedSource});
            }

            std::size_t BatchOf(Graph::Pass pass) const
            {
                const auto& batches = RenderGraph.GetCompiledPlan().Batches;
                for (std::size_t i = 0; i < batches.size(); ++i)
                    if (std::find(batches[i].Passes.begin(), batches[i].Passes.end(), pass) != batches[i].Passes.end())
                        return i;
                throw std::logic_error("UI graph omitted a live batch");
            }

            std::vector<std::unique_ptr<Batch>> Passes;
            Graph RenderGraph;
            Graph::ExecutionBindings Bindings;
        };

        CanvasSize TextureSize(const GDX12Resource& texture)
        {
            if (!texture.D3DResource) throw std::invalid_argument("UI primary target has no allocation");
            const auto desc = texture.D3DResource->GetDesc();
            if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Format != OffscreenFormat ||
                desc.DepthOrArraySize != 1 || desc.MipLevels != 1 || desc.SampleDesc.Count != 1)
                throw std::invalid_argument("UI primary target requires a single RGBA8 texture");
            return {static_cast<UINT>(desc.Width), desc.Height};
        }

        D3D12_RESOURCE_DESC TextureDesc(CanvasSize size)
        {
            return CD3DX12_RESOURCE_DESC::Tex2D(OffscreenFormat, size.Width, size.Height, 1, 1);
        }
    }

    UIOffloadSettings ReadUIOffloadSettings()
    {
        return {Enabled.GetValue(), Scope.GetValue(), static_cast<CanvasId>(Selected.GetValue()), Freshness.GetValue(),
            UpdateHz.GetValue(), MaxAgeMs.GetValue()};
    }

    bool RequestsUIOffload(const UIOffloadSettings& settings, CanvasId surface)
    {
        return settings.Enabled && (settings.Scope == UIOffloadScope::Whole ||
            (settings.Scope == UIOffloadScope::Canvas && surface != CanvasId::Invalid && settings.Selected == surface));
    }

    SnapshotReadMode GetUIOffloadReadMode(const UIOffloadSettings& settings, CanvasId surface)
    {
        return surface != CanvasId::Invalid && settings.Freshness == UIVisualFreshness::Latest
            ? SnapshotReadMode::LatestCompleted : SnapshotReadMode::ExactVersion;
    }

    bool UIUpdateDue(double nowSeconds, std::optional<double> lastAdmissionSeconds, double updateHz)
    {
        return updateHz == 0 || !lastAdmissionSeconds || nowSeconds - *lastAdmissionSeconds >= 1.0 / updateHz;
    }

    UISnapshotAge GetUISnapshotAge(double nowSeconds, double capturedSeconds, double maxAgeMs)
    {
        const double age = (nowSeconds - capturedSeconds) * 1000;
        return {age, maxAgeMs > 0 && age > maxAgeMs};
    }

    struct CrossAdapterUI::Impl
    {
        enum class Stage { Effects, Raster, ExportPrepared, Export, ImportPrepared };
        enum class Advance { Blocked, Progressed, Published };

        struct Publication
        {
            TransferVersion Version;
            FrameSubmissionId Captured;
            double CapturedSeconds = 0;
        };

        struct Job
        {
            Publication Capture;
            std::shared_ptr<const CanvasFrame> Canvas;
            std::shared_ptr<const DrawData> Draw;
            PhysicalView Effects, Raster;
            std::shared_ptr<Transport> Bank;
            GDX12Snapshot::Reservation Credit;
            GDX12Snapshot::ImportPlan Import;
            Stage Progress = Stage::Effects;
            CompletionPoint Ready;
            std::shared_ptr<const void> EffectOwner, RasterOwner, CompositeOwner;
        };

        struct Stream
        {
            explicit Stream(CanvasId key) : Key(key) { Pending.reserve(GDX12Snapshot::SlotCount); }
            CanvasId Key;
            TransferId Id = TransferId::Invalid;
            Epoch LayoutEpoch = Epoch::Invalid;
            CanvasSize Size;
            std::uint64_t Revision = 0;
            std::uint64_t Sequence = 0;
            std::uint64_t Footprint = 0;
            std::uint64_t Deferred = 0, Dropped = 0;
            bool Configured = false;
            std::optional<double> LastAdmission;
            FrameSubmissionId LastAdmissionFrame = FrameSubmissionId::Invalid;
            UIPlacementStatus Status;
            std::shared_ptr<Transport> Bank;
            std::vector<std::shared_ptr<Job>> Pending;
            std::array<Publication, GDX12Snapshot::SlotCount> Publications{};
        };

        struct TargetIdentity
        {
            std::weak_ptr<GDX12Resource> Tracker;
            ID3D12Resource* Native;
            ResourceIdentity Identity;
        };

        Impl(RenderMGPURuntime& runtime, GDX12Device& primary, GDX12Device& secondary, const std::vector<CanvasId>& canvases)
            : Runtime(runtime), Primary(primary), Secondary(secondary), DirectA{primary.GetSubmissionAdapterId(), QueueKind::Direct},
              DirectB{secondary.GetSubmissionAdapterId(), QueueKind::Direct}, ComputeB{DirectB.Adapter, QueueKind::Compute},
              CopyB{DirectB.Adapter, QueueKind::Copy}, CopyA{DirectA.Adapter, QueueKind::Copy}
        {
            if (!DirectA.IsValid() || !DirectB.IsValid() || DirectA.Adapter == DirectB.Adapter)
                throw std::invalid_argument("UI offload requires two live runtime adapters");
            Streams.emplace_back(CanvasId::Invalid);
            for (const auto canvas : canvases)
            {
                if (Find(canvas)) throw std::invalid_argument("UI offload requires unique nonzero canvas IDs");
                Streams.emplace_back(canvas);
            }
        }

        Stream* Find(CanvasId id)
        {
            for (auto& stream : Streams)
                if (stream.Key == id) return &stream;
            return nullptr;
        }

        void Reset(Stream& stream)
        {
            for (const auto& job : stream.Pending)
            {
                Require(stream.Bank->Cancel(job->Credit.Publication), "Cannot cancel an unsubmitted UI stage");
                ++stream.Dropped;
            }
            stream.Pending.clear();
            stream.LayoutEpoch = Runtime.NewEpoch();
            if (stream.Bank) Require(stream.Bank->Reset(stream.LayoutEpoch), "UI bank has an unbound stage");
            stream.Size = {};
            stream.Revision = 0;
            stream.Configured = false;
            stream.Publications = {};
        }

        bool Configure(Stream& stream, CanvasSize size, std::uint64_t revision)
        {
            if (stream.LayoutEpoch == Epoch::Invalid || stream.Size != size || stream.Revision != revision)
            {
                Reset(stream);
                stream.Size = size;
                stream.Revision = revision;
            }
            if (stream.Configured) return true;
            const auto desc = TextureDesc(size);
            if (stream.Id == TransferId::Invalid) stream.Id = Runtime.NewTransferId();
            if (!stream.Bank)
                stream.Bank = Runtime.CreateBank(stream.Id, stream.LayoutEpoch, DirectB.Adapter, DirectA.Adapter, desc);
            else if (!Runtime.TryReconfigureBank(*stream.Bank, desc)) return false;
            Secondary.GetDevice()->GetCopyableFootprints(&desc, 0, 1, 0, nullptr, nullptr, nullptr, &stream.Footprint);
            stream.Configured = true;
            return true;
        }

        bool Due(const Stream& stream) const
        {
            return stream.LastAdmissionFrame != Frame && UIUpdateDue(NowSeconds, stream.LastAdmission, Settings.UpdateHz);
        }

        void MarkAdmitted(Stream& stream)
        {
            stream.LastAdmission = NowSeconds;
            stream.LastAdmissionFrame = Frame;
        }

        PhysicalView MakeSurface(Stream& stream, CanvasSize size, D3D12_RESOURCE_FLAGS flags)
        {
            auto desc = TextureDesc(size);
            desc.Flags = flags;
            const D3D12_CLEAR_VALUE clear{OffscreenFormat, {0, 0, 0, 0}};
            auto tracker = std::make_shared<GDX12Resource>();
            tracker->D3DResource = Secondary.CreateResource(D3D12_HEAP_TYPE_DEFAULT, desc, D3D12_RESOURCE_STATE_COMMON,
                flags == D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET ? &clear : nullptr);
            return {std::move(tracker), {Runtime.NewResourceId(), DirectB.Adapter, stream.LayoutEpoch, stream.Id,
                Runtime.NewAllocationId()}};
        }

        PhysicalView BindTarget(std::shared_ptr<GDX12Resource> target, CompletionPoint prefix)
        {
            auto& queue = *Primary.GetCommandQueue();
            const auto issued = queue.GetLastCompletionPoint();
            if (!target || !target->D3DResource || target->GetCurrentState() != D3D12_RESOURCE_STATE_COMMON ||
                !prefix.IsValid() || !(prefix.Timeline() == issued.Timeline()) || prefix.Value() != issued.Value())
                throw std::invalid_argument("UI finalizer requires the primary target in COMMON and the current DIRECT prefix");
            auto* native = target->D3DResource.Get();
            TargetIds.erase(std::remove_if(TargetIds.begin(), TargetIds.end(),
                [](const TargetIdentity& entry) { return entry.Tracker.expired(); }), TargetIds.end());
            for (auto& entry : TargetIds)
            {
                const auto tracker = entry.Tracker.lock();
                if (tracker == target && entry.Native == native) return {std::move(target), entry.Identity};
                if (entry.Native == native && tracker != target)
                    throw std::invalid_argument("UI primary target has competing state trackers");
            }
            const ResourceIdentity identity{Runtime.NewResourceId(), DirectA.Adapter, Runtime.NewEpoch(),
                TransferId::Invalid, Runtime.NewAllocationId()};
            TargetIds.push_back({target, native, identity});
            return {std::move(target), identity};
        }

        std::shared_ptr<Job> MakeJob(Stream& stream, std::shared_ptr<const CanvasFrame> canvas,
            std::shared_ptr<const DrawData> draw)
        {
            if (stream.Sequence == UINT64_MAX) throw std::overflow_error("UI producer invocation exhausted");
            auto job = std::make_shared<Job>();
            job->Capture = {{stream.Id, stream.LayoutEpoch, ProducerInvocationId{++stream.Sequence}}, Frame, NowSeconds};
            job->Canvas = std::move(canvas);
            job->Draw = std::move(draw);
            job->Bank = stream.Bank;
            if (job->Canvas)
            {
                const auto profile = GetCanvasProfile(job->Canvas->LogicalSize, job->Canvas->Quality);
                job->Effects = MakeSurface(stream, profile.EffectsSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
                if (!Effects) Effects = std::make_unique<CanvasEffects>(Secondary);
            }
            job->Raster = MakeSurface(stream, stream.Size, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
            if (!Raster) Raster = std::make_unique<OffscreenRenderer>(Secondary,
                static_cast<unsigned>(Streams.size() * GDX12Snapshot::SlotCount));
            return job;
        }

        Graph::Pass AddEffects(Plan& plan, const std::shared_ptr<Job>& job, Graph::Resource effect)
        {
            return plan.Add("UI effects", ComputeB, {{effect, Mode::Write, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, job->Capture.Version}},
                [this, job, effect](GDX12CommandList& list, const Graph::BoundResources& views)
                {
                    Require(views.Get(effect) == job->Effects.Resource->D3DResource.Get(), "UI effect binding changed");
                    job->EffectOwner = Effects->Record(list, *job->Canvas, job->Effects.Resource->D3DResource);
                });
        }

        Graph::Pass AddRaster(Plan& plan, const std::shared_ptr<Job>& job, Graph::Resource raster,
            std::optional<Graph::Resource> effect)
        {
            std::vector<Graph::Access> accesses{{raster, Mode::Write, D3D12_RESOURCE_STATE_RENDER_TARGET, job->Capture.Version}};
            if (effect) accesses.push_back({*effect, Mode::Read, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, job->Capture.Version});
            return plan.Add("UI raster", DirectB, std::move(accesses),
                [this, job, raster, effect](GDX12CommandList& list, const Graph::BoundResources& views)
                {
                    Require(views.Get(raster) == job->Raster.Resource->D3DResource.Get(), "UI raster binding changed");
                    std::vector<TextureBinding> textures;
                    if (effect)
                    {
                        Require(views.Get(*effect) == job->Effects.Resource->D3DResource.Get(), "UI effect binding changed");
                        textures.push_back({CanvasEffectTextureId, job->Effects.Resource->D3DResource});
                    }
                    job->RasterOwner = Raster->TryRecord(list, job->Raster.Resource->D3DResource, job->Draw, textures);
                    if (!job->RasterOwner) throw RasterBusy{};
                });
        }

        Graph::Pass AddExport(Plan& plan, const std::shared_ptr<Job>& job, Graph::Resource raster, Graph::Resource shared)
        {
            return plan.Add("UI export", CopyB, {{raster, Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE, job->Capture.Version},
                {shared, Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST, job->Capture.Version}},
                [job](GDX12CommandList& list, const Graph::BoundResources&)
                { job->Bank->RecordExportCopies(job->Credit.Publication, *list.GetCommandList().Get()); });
        }

        Graph::Pass AddImport(Plan& plan, const std::shared_ptr<Job>& job, Graph::Resource shared, Graph::Resource local)
        {
            return plan.Add("UI import", CopyA, {{shared, Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE, job->Capture.Version},
                {local, Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST, job->Capture.Version}},
                [job](GDX12CommandList& list, const Graph::BoundResources&)
                { job->Bank->RecordImportCopies(job->Credit.Publication, *list.GetCommandList().Get()); });
        }

        Graph::Pass AddMaterialCopy(Plan& plan, Graph::Resource local, Graph::Resource target, TransferVersion version)
        {
            return plan.Add("UI material copy", DirectA, {{local, Mode::Read, D3D12_RESOURCE_STATE_COPY_SOURCE, version},
                {target, Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST}},
                [local, target](GDX12CommandList& list, const Graph::BoundResources& views)
                { list.CopyResource(views.Get(target), views.Get(local)); });
        }

        void SetDisplayed(Stream& stream, const Publication& capture)
        {
            auto& status = stream.Status;
            status.ActiveAdapter = DirectB.Adapter;
            status.Displayed = capture.Version;
            status.FrameAge = static_cast<std::uint64_t>(Frame) - static_cast<std::uint64_t>(capture.Captured);
            status.CapturedSeconds = capture.CapturedSeconds;
            status.Age = GetUISnapshotAge(NowSeconds, capture.CapturedSeconds, Settings.MaxAgeMs);
            status.PayloadBytes = static_cast<std::uint64_t>(stream.Size.Width) * stream.Size.Height * 4;
            status.FootprintBytes = stream.Footprint;
            status.MaterialCopyBytes = stream.Key == CanvasId::Invalid ? 0 : status.PayloadBytes;
            status.Fallback = {};
        }

        std::optional<CompletionPoint> Exact(Stream& stream, std::shared_ptr<const CanvasFrame> canvas,
            std::shared_ptr<const DrawData> draw, const PhysicalView& output)
        {
            auto job = MakeJob(stream, std::move(canvas), std::move(draw));
            auto& bank = *job->Bank;
            auto& direct = *Primary.GetCommandQueue();
            job->Credit = bank.Reserve(job->Capture.Version, SnapshotReadMode::ExactVersion, *Secondary.GetCommandQueue(),
                job->Raster.Resource->D3DResource, job->Raster.Identity.Allocation);
            if (job->Credit.Status != Result::Accepted)
            {
                if (job->Credit.Status == Result::Deferred)
                {
                    Require(bank.CancelDeferredExact(job->Capture.Version), "UI Exact deferred credit was lost");
                    ++stream.Deferred;
                }
                return {};
            }
            const auto ticket = job->Credit.Publication;
            try
            {
                Require(bank.PrepareExact(ticket), "Cannot prepare reserved UI Exact ticket");
                const auto reader = bank.PrepareExactReader(ticket, direct);
                if (!reader)
                {
                    Require(bank.Cancel(ticket), "Cannot cancel unused UI Exact ticket");
                    ++stream.Deferred;
                    return {};
                }
                const auto& shared = bank.GetSlots()[job->Credit.SharedSlot];
                Plan plan;
                plan.Bindings.KeepAlive = {job};
                std::optional<Graph::Resource> effect;
                if (job->Canvas)
                {
                    effect = plan.Bind("UI effect output", job->Effects);
                    AddEffects(plan, job, *effect);
                }
                const auto raster = plan.Bind("UI raster output", job->Raster);
                const auto exported = plan.Bind("UI shared export", shared.Export, false, true);
                const auto imported = plan.Bind("UI shared import", shared.Import, false, true);
                const auto local = plan.Bind("UI imported RGBA", reader->View);
                const auto target = plan.Bind("UI primary output", output, !job->Canvas);
                const auto produce = AddRaster(plan, job, raster, effect);
                const auto send = AddExport(plan, job, raster, exported);
                const auto receive = AddImport(plan, job, imported, local);
                Graph::Pass consume;
                if (job->Canvas) consume = AddMaterialCopy(plan, local, target, job->Capture.Version);
                else
                {
                    if (!Composite) Composite = std::make_unique<ScreenComposite>(Primary);
                    // The shared root-signature helper exposes this SRV to every shader stage.
                    consume = plan.Add("UI screen composite", DirectA,
                        {{local, Mode::Read, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, job->Capture.Version},
                         {target, Mode::ReadWrite, D3D12_RESOURCE_STATE_RENDER_TARGET}},
                        [this, job, local, target, source = reader->View.Resource, output](GDX12CommandList& list,
                            const Graph::BoundResources& views)
                        {
                            Require(views.Get(local) == source->D3DResource.Get() && views.Get(target) == output.Resource->D3DResource.Get(),
                                "UI composite bindings changed");
                            job->CompositeOwner = Composite->Record(list, source->D3DResource, output.Resource->D3DResource);
                        });
                }
                plan.RenderGraph.CompilePlan();
                const auto producerBatch = plan.BatchOf(produce), exportBatch = plan.BatchOf(send),
                    importBatch = plan.BatchOf(receive), readerBatch = plan.BatchOf(consume);
                const auto receipt = Runtime.TrySubmit(plan.RenderGraph, std::move(plan.Bindings),
                    [&](const Graph::ExecutionReceipt& done)
                    {
                        const auto& points = done.Batches;
                        bank.CommitExact(ticket, points.at(producerBatch), points.at(exportBatch), points.at(exportBatch),
                            points.at(importBatch), points.at(importBatch));
                        Require(bank.BindReader(reader->Id, points.at(readerBatch)), "Cannot bind UI Exact reader receipt");
                        Require(bank.ReleaseExact(ticket), "Cannot release UI Exact pin");
                    });
                if (!receipt)
                {
                    Require(bank.Cancel(ticket), "Cannot cancel unsubmitted UI Exact ticket");
                    ++stream.Deferred;
                    return {};
                }
                MarkAdmitted(stream);
                SetDisplayed(stream, job->Capture);
                return receipt->Batches[readerBatch];
            }
            catch (...)
            {
                if (!Runtime.IsStopped()) Require(bank.Cancel(ticket), "Cannot cancel failed UI Exact recording");
                throw;
            }
        }

        bool Completed(CompletionPoint point)
        {
            auto* queue = Runtime.GetQueue(point.Timeline().Producer.Location);
            Require(queue && queue->GetTimelineIdentity() == point.Timeline(), "UI stage lost its issuing timeline");
            return point.IsReachedBy(queue->GetCompletionProgress());
        }

        void StartLatest(Stream& stream, std::shared_ptr<const CanvasFrame> canvas)
        {
            const auto draw = canvas->Draw;
            auto job = MakeJob(stream, std::move(canvas), draw);
            auto& bank = *job->Bank;
            job->Credit = bank.Reserve(job->Capture.Version, SnapshotReadMode::LatestCompleted, *Secondary.GetCommandQueue(),
                job->Raster.Resource->D3DResource, job->Raster.Identity.Allocation);
            if (job->Credit.Status != Result::Accepted)
            {
                if (job->Credit.Status == Result::Deferred) ++stream.Deferred;
                return;
            }
            const auto ticket = job->Credit.Publication;
            try
            {
                Plan plan;
                plan.Bindings.KeepAlive = {job};
                const auto effect = plan.Bind("UI effect output", job->Effects);
                const auto pass = AddEffects(plan, job, effect);
                plan.RenderGraph.CompilePlan();
                const auto batch = plan.BatchOf(pass);
                const auto receipt = Runtime.TrySubmit(plan.RenderGraph, std::move(plan.Bindings));
                if (!receipt)
                {
                    Require(bank.Cancel(ticket), "Cannot cancel unused UI Latest ticket");
                    ++stream.Deferred;
                    return;
                }
                job->Ready = receipt->Batches[batch];
                stream.Pending.push_back(std::move(job));
                MarkAdmitted(stream);
            }
            catch (...)
            {
                if (!Runtime.IsStopped()) Require(bank.Cancel(ticket), "Cannot cancel failed UI effects recording");
                throw;
            }
        }

        Advance AdvanceLatest(Stream& stream, const std::shared_ptr<Job>& job)
        {
            if (!Completed(job->Ready)) return Advance::Blocked;
            auto& bank = *job->Bank;
            const auto ticket = job->Credit.Publication;
            const auto& shared = bank.GetSlots()[job->Credit.SharedSlot];
            Plan plan;
            plan.Bindings.KeepAlive = {job};
            if (job->Progress == Stage::Effects)
            {
                const auto effect = plan.Bind("UI finished effects", job->Effects, true);
                plan.Import(effect, job->Capture.Version, job->Ready, job);
                const auto raster = plan.Bind("UI raster output", job->Raster);
                const auto pass = AddRaster(plan, job, raster, effect);
                plan.RenderGraph.CompilePlan();
                const auto batch = plan.BatchOf(pass);
                const auto receipt = Runtime.TrySubmit(plan.RenderGraph, std::move(plan.Bindings),
                    [&](const Graph::ExecutionReceipt& done)
                    { Require(bank.BindProducer(ticket, done.Batches[batch]), "Cannot bind UI Latest producer"); });
                if (receipt)
                {
                    job->Progress = Stage::Raster;
                    job->Ready = receipt->Batches[batch];
                    job->EffectOwner.reset();
                    return Advance::Progressed;
                }
                ++stream.Deferred;
                return Advance::Blocked;
            }

            if (job->Progress == Stage::Raster)
            {
                const auto ready = bank.PrepareExport(ticket);
                Require(ready && ready->Timeline() == job->Ready.Timeline() && ready->Value() == job->Ready.Value(),
                    "Cannot prepare UI Latest export");
                job->Progress = Stage::ExportPrepared;
                job->RasterOwner.reset();
            }
            if (job->Progress == Stage::ExportPrepared)
            {
                const auto raster = plan.Bind("UI finished raster", job->Raster, true);
                plan.Import(raster, job->Capture.Version, job->Ready, job);
                const auto exported = plan.Bind("UI shared export", shared.Export, false, true);
                const auto pass = AddExport(plan, job, raster, exported);
                plan.RenderGraph.CompilePlan();
                const auto batch = plan.BatchOf(pass);
                const auto receipt = Runtime.TrySubmit(plan.RenderGraph, std::move(plan.Bindings),
                    [&](const Graph::ExecutionReceipt& done)
                    { bank.CommitExport(ticket, done.Batches[batch], done.Batches[batch]); });
                if (receipt)
                {
                    job->Progress = Stage::Export;
                    job->Ready = receipt->Batches[batch];
                    return Advance::Progressed;
                }
                ++stream.Deferred;
                return Advance::Blocked;
            }

            if (job->Progress == Stage::Export)
            {
                job->Import = bank.PrepareImport(ticket);
                if (job->Import.Status == Result::Deferred) ++stream.Deferred;
                if (job->Import.Status == Result::Deferred || job->Import.Status == Result::NotReady) return Advance::Blocked;
                Require(job->Import.Status == Result::Accepted, "Cannot prepare UI Latest import");
                job->Progress = Stage::ImportPrepared;
            }
            const auto imported = plan.Bind("UI finished shared export", shared.Import, true, true);
            plan.Import(imported, job->Capture.Version, job->Import.ExportReady, job,
                Runtime.GetQueue(CopyB), shared.Export.Identity);
            const auto local = plan.Bind("UI imported RGBA", bank.GetSlots()[job->Import.LocalSlot].Local);
            const auto pass = AddImport(plan, job, imported, local);
            plan.RenderGraph.CompilePlan();
            const auto batch = plan.BatchOf(pass);
            const auto receipt = Runtime.TrySubmit(plan.RenderGraph, std::move(plan.Bindings),
                [&](const Graph::ExecutionReceipt& done)
                { bank.CommitImport(ticket, done.Batches[batch], done.Batches[batch]); });
            if (receipt)
            {
                stream.Publications[job->Import.LocalSlot] = job->Capture;
                return Advance::Published;
            }
            ++stream.Deferred;
            return Advance::Blocked;
        }

        std::optional<CompletionPoint> Latest(Stream& stream, const PhysicalView& output)
        {
            auto& bank = *stream.Bank;
            const auto snapshot = bank.SelectLatest(Frame).Current;
            if (!snapshot.IsValid()) return {};
            const auto metadata = stream.Publications.at(snapshot.LocalSlot);
            Require(metadata.Version == snapshot.Version && metadata.Captured != FrameSubmissionId::Invalid,
                "UI Latest snapshot has no capture metadata");
            const auto reader = bank.AcquireReader(snapshot.Publication, *Primary.GetCommandQueue());
            if (!reader)
            {
                ++stream.Deferred;
                return {};
            }
            try
            {
                Plan plan;
                plan.Bindings.KeepAlive = {stream.Bank};
                const auto local = plan.Bind("UI latest completed RGBA", reader->View, true);
                plan.Import(local, snapshot.Version, snapshot.ImportReady, stream.Bank);
                const auto target = plan.Bind("UI primary material output", output);
                const auto pass = AddMaterialCopy(plan, local, target, snapshot.Version);
                plan.RenderGraph.CompilePlan();
                const auto batch = plan.BatchOf(pass);
                const auto receipt = Runtime.TrySubmit(plan.RenderGraph, std::move(plan.Bindings),
                    [&](const Graph::ExecutionReceipt& done)
                    { Require(bank.BindReader(reader->Lease.Id, done.Batches[batch]), "Cannot bind UI Latest reader"); });
                if (!receipt)
                {
                    Require(bank.CancelReader(reader->Lease.Id), "Cannot cancel unused UI Latest reader");
                    ++stream.Deferred;
                    return {};
                }
                SetDisplayed(stream, metadata);
                return receipt->Batches[batch];
            }
            catch (...)
            {
                if (!Runtime.IsStopped()) Require(bank.CancelReader(reader->Lease.Id), "Cannot cancel failed UI Latest reader");
                throw;
            }
        }

        template<class F>
        std::optional<CompletionPoint> TryRoute(Stream& stream, F action)
        {
            try { return action(); }
            catch (const RasterBusy&)
            {
                ++stream.Deferred;
                stream.Status.Fallback = "UI raster ring busy";
            }
            catch (const DxException& error)
            {
                if (Runtime.IsStopped()) throw;
                OutputDebugStringW(error.ToString().c_str());
                stream.Status.Fallback = "UI device or allocation unavailable";
            }
            catch (const std::invalid_argument& error)
            {
                if (Runtime.IsStopped()) throw;
                OutputDebugStringA(error.what());
                stream.Status.Fallback = "UI texture or route unsupported";
            }
            catch (const std::runtime_error& error)
            {
                if (Runtime.IsStopped()) throw;
                OutputDebugStringA(error.what());
                stream.Status.Fallback = "UI backend unavailable";
            }
            return {};
        }

        RenderMGPURuntime& Runtime;
        GDX12Device& Primary;
        GDX12Device& Secondary;
        const Placement DirectA, DirectB, ComputeB, CopyB, CopyA;
        FrameSubmissionId Frame = FrameSubmissionId::Invalid;
        UIOffloadSettings Settings;
        double NowSeconds = 0;
        std::vector<Stream> Streams;
        std::vector<TargetIdentity> TargetIds;
        std::unique_ptr<OffscreenRenderer> Raster;
        std::unique_ptr<CanvasEffects> Effects;
        std::unique_ptr<ScreenComposite> Composite;
    };

    CrossAdapterUI::CrossAdapterUI(RenderMGPURuntime& runtime, GDX12Device& primary, GDX12Device& secondary,
        const std::vector<CanvasId>& canvases)
        : _impl(std::make_unique<Impl>(runtime, primary, secondary, canvases))
    {
    }

    CrossAdapterUI::~CrossAdapterUI() = default;

    void CrossAdapterUI::BeginFrame(FrameSubmissionId frame, UIOffloadSettings settings, double nowSeconds)
    {
        auto& state = *_impl;
        if (frame <= state.Frame) throw std::invalid_argument("UI frame IDs must increase");
        if (!std::isfinite(nowSeconds) || nowSeconds < state.NowSeconds || nowSeconds > (std::numeric_limits<double>::max)() / 1000 ||
            !ValidUpdateHz(settings.UpdateHz) || !ValidMaxAgeMs(settings.MaxAgeMs))
            throw std::invalid_argument("UI requires a monotonic clock and valid cadence controls");
        state.Frame = frame;
        state.Settings = settings;
        state.NowSeconds = nowSeconds;
        for (auto& stream : state.Streams)
        {
            const bool requested = RequestsUIOffload(settings, stream.Key);
            const auto mode = GetUIOffloadReadMode(settings, stream.Key);
            if (stream.Status.Requested && (!requested || stream.Status.Mode != mode) &&
                stream.LayoutEpoch != Epoch::Invalid) state.Reset(stream);
            stream.Status = {};
            stream.Status.Requested = requested;
            stream.Status.ActiveAdapter = state.DirectA.Adapter;
            stream.Status.Mode = mode;
            if (requested) stream.Status.Fallback = "UI secondary output not ready";
        }
    }

    void CrossAdapterUI::Invalidate(CanvasId surface)
    {
        auto* stream = _impl->Find(surface);
        if (!stream) throw std::invalid_argument("Unknown UI surface");
        if (stream->LayoutEpoch != Epoch::Invalid) _impl->Reset(*stream);
        stream->Status.ActiveAdapter = _impl->DirectA.Adapter;
        stream->Status.Displayed = {};
        stream->Status.CapturedSeconds = 0;
        stream->Status.Age = {};
        stream->Status.FrameAge = stream->Status.PayloadBytes = stream->Status.FootprintBytes = stream->Status.MaterialCopyBytes = 0;
        stream->Status.Fallback = stream->Status.Requested ? "UI surface invalidated" : std::string_view{};
    }

    UIPlacementStatus CrossAdapterUI::GetStatus(CanvasId surface) const
    {
        if (const auto* stream = _impl->Find(surface))
        {
            auto status = stream->Status;
            status.Deferred = stream->Deferred;
            status.Dropped = stream->Dropped;
            return status;
        }
        UIPlacementStatus status;
        status.Requested = RequestsUIOffload(_impl->Settings, surface);
        status.ActiveAdapter = _impl->DirectA.Adapter;
        status.Mode = GetUIOffloadReadMode(_impl->Settings, surface);
        status.Fallback = "Unknown CanvasId";
        return status;
    }

    void CrossAdapterUI::PumpLatest(std::shared_ptr<const CanvasFrame> frame, bool allowNewPublication)
    {
        if (!frame || !frame->Draw || frame->Id == CanvasId::Invalid)
            throw std::invalid_argument("Latest UI requires a frozen canvas frame");
        auto& state = *_impl;
        auto* stream = state.Find(frame->Id);
        if (!stream || !stream->Status.Requested || stream->Status.Mode != SnapshotReadMode::LatestCompleted) return;
        if (!state.Runtime.GetQueue(state.ComputeB))
        {
            stream->Status.Fallback = "Secondary UI COMPUTE unavailable";
            return;
        }
        state.TryRoute(*stream, [&]() -> std::optional<CompletionPoint>
        {
            const auto profile = GetCanvasProfile(frame->LogicalSize, frame->Quality);
            if (!state.Configure(*stream, profile.RasterSize, frame->SurfaceRevision)) return {};
            for (auto next = stream->Pending.begin(); next != stream->Pending.end();)
            {
                try
                {
                    auto progress = state.AdvanceLatest(*stream, *next);
                    while (progress == Impl::Advance::Progressed) progress = state.AdvanceLatest(*stream, *next);
                    if (progress == Impl::Advance::Published) next = stream->Pending.erase(next);
                    else ++next;
                }
                catch (const RasterBusy&)
                {
                    // Completed effects remain eligible for the next free raster slot.
                    throw;
                }
                catch (...)
                {
                    if (!state.Runtime.IsStopped())
                    {
                        Require(stream->Bank->Cancel((*next)->Credit.Publication), "Cannot cancel unsubmitted UI Latest stage");
                        stream->Pending.erase(next);
                        ++stream->Dropped;
                    }
                    throw;
                }
            }
            if (allowNewPublication && state.Due(*stream))
            {
                if (stream->Pending.size() < GDX12Snapshot::SlotCount) state.StartLatest(*stream, std::move(frame));
                else ++stream->Deferred;
            }
            return {};
        });
    }

    std::optional<CompletionPoint> CrossAdapterUI::SubmitCanvas(const CanvasFrame& frame,
        std::shared_ptr<GDX12Resource> materialTarget, CompletionPoint prefix)
    {
        auto& state = *_impl;
        auto* stream = state.Find(frame.Id);
        if (!stream || !stream->Status.Requested || !frame.Draw || frame.Id == CanvasId::Invalid) return {};
        if (!state.Runtime.GetQueue(state.ComputeB))
        {
            stream->Status.Fallback = "Secondary UI COMPUTE unavailable";
            return {};
        }
        return state.TryRoute(*stream, [&]() -> std::optional<CompletionPoint>
        {
            const auto output = state.BindTarget(std::move(materialTarget), prefix);
            const auto profile = GetCanvasProfile(frame.LogicalSize, frame.Quality);
            if (TextureSize(*output.Resource) != profile.RasterSize)
                throw std::invalid_argument("UI material target differs from the frozen canvas profile");
            if (!state.Configure(*stream, profile.RasterSize, frame.SurfaceRevision))
            {
                stream->Status.Fallback = "UI bank waiting for old layout retirement";
                return {};
            }
            if (stream->Status.Mode == SnapshotReadMode::LatestCompleted)
                return state.Latest(*stream, output);
            if (!state.Due(*stream))
            {
                stream->Status.Fallback = "UI producer cadence cap";
                return {};
            }
            const auto point = state.Exact(*stream, std::make_shared<const CanvasFrame>(frame), frame.Draw, output);
            if (!point) stream->Status.Fallback = "UI Exact capacity exhausted";
            return point;
        });
    }

    std::optional<CompletionPoint> CrossAdapterUI::SubmitScreen(std::shared_ptr<const DrawData> draw,
        std::shared_ptr<GDX12Resource> backbuffer, CompletionPoint prefix)
    {
        auto& state = *_impl;
        auto& stream = *state.Find(CanvasId::Invalid);
        if (!stream.Status.Requested || !draw) return {};
        return state.TryRoute(stream, [&]() -> std::optional<CompletionPoint>
        {
            const auto output = state.BindTarget(std::move(backbuffer), prefix);
            if (!state.Configure(stream, TextureSize(*output.Resource), 1))
            {
                stream.Status.Fallback = "UI bank waiting for old screen size retirement";
                return {};
            }
            const auto point = state.Exact(stream, {}, std::move(draw), output);
            if (!point) stream.Status.Fallback = "UI Exact capacity exhausted";
            return point;
        });
    }
}

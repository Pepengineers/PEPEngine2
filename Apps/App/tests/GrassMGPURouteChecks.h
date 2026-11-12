#pragma once

#include "../src/Rendering/Grass/CrossAdapterGrass.h"
#include "App.Base/Rendering/RenderMGPURuntime.h"
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include <stdexcept>

// Integration harness supplies two real adapters, injected COPY queues and prepared primary
// frame constants. Queue drains below are test controls, outside the production controller.
static void CheckGrassMGPURoute(RenderMGPURuntime& runtime, GDX12DeviceResources& primary,
    GDX12Device& secondary, const RenderPipelineCommonData& common)
{
    using namespace GDX12Submission;
    using Clock = CrossAdapterGrass::Clock;
    using namespace std::chrono_literals;
    const auto require = [](bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    };
    auto& direct = *primary.Device->GetCommandQueue();
    auto& copyA = *runtime.GetQueue({primary.Device->GetSubmissionAdapterId(), QueueKind::Copy});
    auto& copyB = *runtime.GetQueue({secondary.GetSubmissionAdapterId(), QueueKind::Copy});
    require(primary.Device->GetSubmissionAdapterId() != secondary.GetSubmissionAdapterId(),
        "Grass MGPU route requires two physical adapters");

    struct QueueGate
    {
        ComPtr<ID3D12Fence> Fence;
        QueueGate(GDX12Device& device, GDX12CommandQueue& queue)
        {
            ThrowIfFailed(device.GetDevice()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&Fence)));
            ThrowIfFailed(queue.GetCommandQueue()->Wait(Fence.Get(), 1));
        }
        ~QueueGate() { Fence->Signal(1); }
        void Open() { ThrowIfFailed(Fence->Signal(1)); }
    };

    CrossAdapterGrass route(runtime, primary, secondary);
    GDX12GrassRenderer renderer(&primary);
    auto controls = GetGrassWindControls();
    controls.GrassEnabled = controls.WindEnabled = true;
    controls.GrassQuality = controls.WindQuality = 2;
    GDX12GrassInstance grass;
    grass.Id = 1;
    grass.WindId = 11;
    grass.RootsCm = {{0, 0, 0}};

    GDX12WindInstance analytic;
    analytic.Id = 11;
    analytic.Implementation = GDX12WindImplementation::Analytic;
    analytic.GridResolution = 8;
    analytic.DeltaSeconds = 1.0f / 60;
    auto projected = analytic;
    projected.Id = 12;
    projected.Implementation = GDX12WindImplementation::ProjectedGrid;
    projected.PressureIterations = 3;
    auto second = grass;
    second.Id = 2;
    second.WindId = projected.Id;

    CrossAdapterGrass::Tick tick;
    tick.InteractionArea = GDX12GrassRenderer::BistroInteractionArea();
    tick.Wind = {analytic, projected};
    tick.DeltaSeconds = analytic.DeltaSeconds;
    const auto start = Clock::now();
    std::shared_ptr<const GDX12GrassRenderer::LocalCheckpoint> checkpoint;
    CompletionPoint checkpointReady;
    for (uint64_t sequence = 1; sequence <= 6; ++sequence)
    {
        tick.Sequence = sequence;
        tick.CapturedAt = start + 16ms * sequence;
        renderer.BeginFrame(common, {grass, second}, tick.Wind, controls, tick.Actors, tick.DeltaSeconds);
        if (sequence == 6) checkpoint = renderer.PrepareLocalCheckpoint();
        auto* list = direct.GetCommandList();
        renderer.PrepareGPU(list);
        direct.ExecuteCommandList(list);
        checkpointReady = direct.SignalCompletion();
        direct.Flush();
    }
    tick.InteractionResetRevision = checkpoint->Layout.InteractionResetRevision;
    const auto sameIdentity = [](const ResourceIdentity& a, const ResourceIdentity& b)
    {
        return a.Id == b.Id && a.Adapter == b.Adapter && a.ResourceEpoch == b.ResourceEpoch &&
            a.Transfer == b.Transfer && a.Allocation == b.Allocation;
    };
    const auto checkpointIdentity = runtime.IdentifyResource(checkpoint->Packed, *primary.Device);
    require(route.TryStart(checkpoint, tick, checkpointReady), "Grass could not seed secondary from a primary checkpoint");

    const GDX12GrassMGPUControls latest{true, false, 0, 10};
    const auto retire = [&](const CrossAdapterGrass::Frame& selected)
    {
        require(!selected.Ready.IsValid(), "Fixture must attach or observe readiness before legacy recording");
        require(selected.CurrentView.Resource.get() == selected.Fields.Current->Presence().Resource &&
            selected.PreviousView.Resource.get() == selected.Fields.Previous->Presence().Resource &&
            selected.CurrentView.Identity.Adapter == primary.Device->GetSubmissionAdapterId(),
            "P2d grass binding does not use the canonical primary field views");
        renderer.BeginFrame(common, {grass, second}, tick.Wind, controls, tick.Actors, tick.DeltaSeconds, &selected.Fields);
        auto* list = direct.GetCommandList();
        renderer.PrepareGPU(list);
        route.RecordFrameRelease(*list);
        direct.ExecuteCommandList(list);
        route.RetireFrame(direct.SignalCompletion());
        direct.Flush();
    };

    const auto beforeImport = copyA.GetLastCompletionPoint();
    ++tick.Sequence;
    tick.CapturedAt += 16ms;
    const auto actor = tick.InteractionArea;
    tick.Actors = {{{actor.x, 0, actor.y, 65}, {actor.x - 15, 0, actor.y, 190}}};
    require(route.Capture(tick), "Grass input after the seed was rejected");
    route.Pump(latest, tick.CapturedAt);
    require(copyA.GetLastCompletionPoint().Value() == beforeImport.Value(),
        "Latest admitted primary COPY before observing completion of remote export");
    {
        auto frozen = route.Select(static_cast<FrameSubmissionId>(1), tick.Sequence, tick.CapturedAt);
        require(frozen.has_value() && frozen->Sequence == 6 &&
            frozen->Fields.Current->GetLayout().InteractionTimeSeconds == checkpoint->Layout.InteractionTimeSeconds,
            "First Latest frame replaced the authoritative primary checkpoint");
        require(sameIdentity(frozen->CurrentView.Identity, checkpointIdentity) &&
            sameIdentity(runtime.IdentifyResource(checkpoint->Packed, *primary.Device), checkpointIdentity),
            "Grass seed changed the runtime canonical checkpoint identity");
        retire(*frozen);
    }

    copyB.Flush();
    runtime.Poll();
    route.Pump(latest, tick.CapturedAt + 16ms);
    copyA.Flush();
    runtime.Poll();
    {
        auto imported = route.Select(static_cast<FrameSubmissionId>(2), tick.Sequence, tick.CapturedAt + 32ms);
        require(imported.has_value() && imported->Sequence == tick.Sequence && imported->OverAge &&
            imported->Fields.Current->GetLayout().InteractionTimeSeconds > checkpoint->Layout.InteractionTimeSeconds,
            "First compatible Latest import did not continue the primary clock");
        retire(*imported);
    }
    ResourceId lastDisplayed = ResourceId::Invalid;
    {
        auto repeated = route.Select(static_cast<FrameSubmissionId>(3), tick.Sequence, tick.CapturedAt + 64ms);
        require(repeated.has_value() && repeated->Sequence == tick.Sequence && repeated->OverAge &&
            repeated->Fields.Current == repeated->Fields.Previous,
            "Grass Latest did not repeat the same compatible local import");
        lastDisplayed = repeated->CurrentView.Identity.Id;
        retire(*repeated);
    }

    auto exact = latest;
    exact.Exact = true;
    exact.UpdateHz = 30;
    ++tick.Sequence;
    tick.CapturedAt += 16ms;
    require(route.Capture(tick), "Grass Exact input was rejected");
    route.Pump(exact, tick.CapturedAt);
    require(route.GetInputStatus().LastIssued + 1 == tick.Sequence,
        "Grass UpdateHz admitted an invocation before its interval");
    QueueGate hold(*primary.Device, copyA);
    route.Pump(exact, tick.CapturedAt + 64ms);
    ResourceId cancelled = ResourceId::Invalid;
    {
        const auto selected = route.Select(static_cast<FrameSubmissionId>(4), tick.Sequence, tick.CapturedAt + 64ms);
        require(selected.has_value() && selected->Exact && selected->Ready.IsValid() &&
            selected->PreviousView.Identity.Id == lastDisplayed,
            "Grass did not stage the pending Exact with the last displayed history");
        cancelled = selected->CurrentView.Identity.Id;
        require(cancelled != lastDisplayed, "Exact test did not stage a distinct allocation");
        route.CancelFrame();
    }
    {
        const auto repeated = route.Select(static_cast<FrameSubmissionId>(4), tick.Sequence, tick.CapturedAt + 64ms, false);
        require(repeated.has_value() && !repeated->Exact && !repeated->Ready.IsValid() &&
            repeated->Sequence + 1 == tick.Sequence &&
            repeated->CurrentView.Identity.Id == lastDisplayed && repeated->PreviousView.Identity.Id == lastDisplayed,
            "Same-frame Exact fallback exposed a pending view or lost the last displayed reader pin");
        retire(*repeated);
    }
    {
        auto selected = route.Select(static_cast<FrameSubmissionId>(5), tick.Sequence, tick.CapturedAt + 80ms);
        require(selected.has_value() && selected->Exact && selected->Sequence == tick.Sequence &&
            selected->CurrentView.Identity.Id == cancelled && selected->PreviousView.Identity.Id == lastDisplayed &&
            selected->CurrentView.Version.IsValid() &&
            selected->CurrentView.Version.Transfer == selected->CurrentView.Identity.Transfer &&
            selected->CurrentView.Version.VersionEpoch == selected->CurrentView.Identity.ResourceEpoch,
            "Exact retry lost its reservation or used a cancelled frame as previous display");
        // The integration bridge attaches selected.Ready to the first native consumer.
        // This fixture releases the test gate and observes completion before legacy recording.
        hold.Open();
        copyA.Flush();
        selected->Ready = {};
        retire(*selected);
    }
    runtime.Poll();

    auto pending = tick;
    ++pending.Sequence;
    pending.CapturedAt += 16ms;
    require(route.Capture(pending), "Grass return input was rejected");
    auto off = latest;
    off.Enabled = false;
    route.Pump(off, pending.CapturedAt + 64ms);
    direct.Flush();
    runtime.Poll();
    auto continuation = route.TakeLocalContinuation();
    require(continuation.has_value() && continuation->Sequence == tick.Sequence &&
        continuation->Pending.Front() && continuation->Pending.Front()->Sequence == pending.Sequence,
        "Grass return lost the authoritative head or its queued next tick");
    renderer.ResumeLocalSolver(std::move(continuation->Solver), continuation->ResetRevision, std::move(continuation->Pending));
    ++pending.Sequence;
    pending.CapturedAt += 16ms;
    renderer.BeginFrame(common, {grass, second}, pending.Wind, controls, pending.Actors, pending.DeltaSeconds);
    const auto returned = renderer.PrepareLocalCheckpoint();
    require(returned->Layout.InteractionTimeSeconds > checkpoint->Layout.InteractionTimeSeconds &&
        returned->Layout.InteractionRevision > checkpoint->Layout.InteractionRevision,
        "Returned primary solver restarted its clock");
    auto* list = direct.GetCommandList();
    renderer.PrepareGPU(list);
    direct.ExecuteCommandList(list);
    const auto returnedReady = direct.SignalCompletion();
    direct.Flush();
    runtime.Poll();
    pending.InteractionResetRevision = returned->Layout.InteractionResetRevision;
    const auto returnedIdentity = runtime.IdentifyResource(returned->Packed, *primary.Device);
    require(route.TryStart(returned, pending, returnedReady),
        "Cancelled Exact or retired display leaked a reader pin across the next epoch");

    {
        const auto anchor = route.Select(static_cast<FrameSubmissionId>(7), pending.Sequence, pending.CapturedAt);
        require(anchor.has_value() && anchor->Sequence == pending.Sequence, "Grass restart lost its primary anchor");
        require(sameIdentity(anchor->CurrentView.Identity, returnedIdentity) &&
            sameIdentity(runtime.IdentifyResource(returned->Packed, *primary.Device), returnedIdentity),
            "Grass restart changed the runtime canonical checkpoint identity");
        retire(*anchor);
    }

    auto input = pending;
    ++input.Sequence;
    input.CapturedAt += 16ms;
    require(route.Capture(input), "Grass completed-candidate input was rejected");
    route.Pump(latest, input.CapturedAt);
    copyB.Flush();
    runtime.Poll();
    route.Pump(latest, input.CapturedAt + 16ms);
    copyA.Flush();
    runtime.Poll();
    const auto completedSequence = input.Sequence;
    const double completedTime = returned->Layout.InteractionTimeSeconds + input.DeltaSeconds;

    QueueGate holdLaterExports(secondary, copyB);
    for (int i = 0; i < 13; ++i)
    {
        ++input.Sequence;
        input.CapturedAt += 16ms;
        require(route.Capture(input), "Grass backlog rejected a compatible input");
    }
    auto backlogExact = latest;
    backlogExact.Exact = true;
    route.Pump(backlogExact, input.CapturedAt);
    require(route.GetInputStatus().LastIssued + 1 == input.Sequence,
        "Grass did not admit three intermediate four-tick publications");
    {
        const auto anchor = route.Select(static_cast<FrameSubmissionId>(8), input.Sequence, input.CapturedAt);
        require(anchor.has_value() && !anchor->Exact && anchor->Sequence == pending.Sequence,
            "Grass Exact adopted an intermediate publication while its demanded tick was deferred");
        retire(*anchor);
    }

    const auto beforeLatestSwitch = copyA.GetLastCompletionPoint();
    route.Pump(latest, input.CapturedAt + 16ms);
    require(copyA.GetLastCompletionPoint().Value() == beforeLatestSwitch.Value(), "Grass Latest imported a gated secondary export");
    {
        const auto completed = route.Select(static_cast<FrameSubmissionId>(9), input.Sequence, input.CapturedAt + 16ms);
        require(completed.has_value() && !completed->Ready.IsValid() && completed->OverAge &&
            completed->Sequence == completedSequence &&
            completed->Fields.Current->GetLayout().InteractionTimeSeconds == completedTime,
            "Grass Latest lost the only completed import's sequence or layout while newer exports were gated");
        retire(*completed);
    }
    holdLaterExports.Open();
}

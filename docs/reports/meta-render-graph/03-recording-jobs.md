# U3 — hierarchical parallel CPU recording on one DIRECT queue

Date: 2026-11-13 (Europe/Moscow). Host: `MidSpec` (requested as MisSpec).
Checkout: `C:/Users/MidSpec/Documents/PEPEngine2`.

## Source and result

Accepted base and initial HEAD: `0ba202a335750fc3f7f1b40ede3253ce2894abd0` (U2), containing accepted U1 `40f986588d36f47fdda49b982bd4f7d00571fceb`. Created local branch `codex/meta-render-graph-u3` from that exact HEAD. Initial tracked/index state was clean. The unrelated untracked `Apps/App/App.vcxproj.user` remains untouched and excluded. Sandbox Git status cannot inspect the inih submodule; status with `--ignore-submodules=all` was used. No submodule changes were made.

Result HEAD is the local commit containing this report (`git log -1 --format=%H -- docs/reports/meta-render-graph/03-recording-jobs.md`). Exact base/result hashes, changed-file SHA256 manifest, commands/results, artifact hashes, log/trace paths and the complete binary patch are in ignored `Build/Verification/meta-render-graph-u3/handoff.json` and `base-to-u3.patch`, generated after commit.

U3 is implemented and locally verified. Acceptance review belongs to the originating chat. No reviewer, subagent, U4 technique migration, other queue/device, GPU async, push, PR or merge was started. No shared Engine/App.Base code changed.

## Recorder and ownership contract

`Graph/MetaGraphRecorder.h/.cpp` introduces `RecordingGroup { Name, Children, Continue }` and `Recorder(JobSystem&, groups, serial, cancellation)`. Trees and callbacks are frozen before launch. A group's enki start task queues its children from the pool worker and returns immediately. Children decrement an atomic counter; a launch sentinel prevents a fast child from scheduling the continuation before all children are registered. The last child queues a **separate** join task, which runs the group's continuation and propagates completion to its parent. This repeats for nested groups. No worker calls Complete/WaitForTask for descendants. Empty groups run their continuation as a leaf.

The existing public `JobSystem::QueueTask` already supports owner/pool-worker submission, so JobSystem and its existing call sites are unchanged. `ParallelJob::Schedule` is not called from workers. Recorder retains every task/node until all logical roots finish and `WaitForTask` has joined every submitted enki start/join task, including its scheduler epilogue. Publishing CPU-ready inside the work callback therefore does not free the task while enki still uses it. Destructor joins even when the caller did not Wait or an exception unwinds the executor.

`Wait(root)` is owner-only. With one scheduler thread it helps through existing `WaitForTask(nullptr)`; with a normal pool it waits on the owner's condition variable, leaving that thread available for submission. Production executor defaults to four scheduler threads including the owner, i.e. three recording workers. CPU tests also exercise the owner-only single-thread scheduler. Single-thread mode proves progress, not recording overlap or early-submission timing.

First callback error is retained and cancels remaining work. External atomic cancellation is cooperative at callback boundaries; already running callbacks finish. Error/cancellation skips continuations that publish useful results. Wait throws, so incomplete/aborted packets cannot be submitted. Shutdown joins tasks before any captured data or lists disappear. Fixture-only fence polling deliberately delays a late bucket to prove early submission; production recording callbacks do not wait for GPU or submit work.

## Executor and D3D12 state contract

`Executor::Execute` now builds one root per packet, a nested bucket group, and distinct prologue/epilogue leaves. All allocator resets, list resets, native resource validation and assignment of output cells happen on the owner before jobs start. Each native list has its own allocator and exactly one writer. Workers record/Close native lists without calling shared resource-state mutation helpers. Pointer arrays are assembled in assigned order before launch, never worker completion order: **prologue → indexed bucket lists → epilogue**. Common transitions occur once in the prologue; bucket lists only touch their frozen, disjoint destination ranges. The epilogue owns packet After barriers.

Owner waits only for the next stable topological packet, then issues its ExecuteCommandLists/Signal. Every packet gets its own successful fence token. CPU-ready and GPU-complete are distinct: readiness means recording and Close succeeded; slot retirement is the last successful queue signal, including suffix. Dependencies are ordered by submission on the same DIRECT queue; there is no GPU async or second queue. Prefix/suffix remain on the owner with the existing frame/timer/UI completion contract. A late packet can still hold later independent packets behind it in deterministic order; U3 does not implement a work-conserving out-of-order submitter.

**U2's single-batch buffer state assumptions were changed explicitly.** `Resource::Decays` and `Schedule::Decays` identify DEFAULT buffers. Compile resets their planned state to COMMON after every packet/Execute boundary and requires final COMMON. Each later use gets a fresh COMMON→required transition. Within the packet's one Execute call, states remain valid across the prologue/buckets/epilogue lists. Thus the two/four buckets never independently transition a shared destination. Upload/readback heaps keep fixed GENERIC_READ/COPY_DEST states. Ordinary target texture states persist across packets and finish at the declared final state. Executor checks physical heap/dimension against Decays and rejects mismatches, CUSTOM heaps and simultaneous-access resources, whose decay semantics need a separate future contract. No runtime shared `_currentState` is mutated by workers.

`Begin(slot)` CPU-waits the last retirement fence **before** the caller may rewrite that slot's upload memory/descriptors or reset its allocators. Resource/descriptor ownership remains explicit and fixed: fixture upload buffers/root descriptors and a stable RTV heap; this is not a new transient range allocator. Resize/shutdown drain before owners release resources. The application reserves 64 lists per each of three frame slots (192 allocator/list pairs, versus U2's 48). Its 12-packet schedule consumes 40 lists: 16 operation/terminal lists plus 24 boundary lists. This intentional fixed memory/CPU overhead is not presented as a performance improvement.

Serial reference uses the same compiled schedule, native list ownership, barriers and packet submission path with `serial=true`; only callback execution becomes depth-first on the owner. Tests run serial/parallel pairs with identical seeds and validate every result byte/pixel against the same exact expected values.

## Errors and partial frames

Before any submission, recording failure/cancellation leaves all tokens zero and prior retirement unchanged. Workers are joined and abandoned open lists are closed, permitting reuse after Begin. A Close failure never publishes readiness or submits that packet; native Close/Signal failure injection is not covered by the fixtures.

After early successful submissions, a later recording error/cancellation preserves those tokens and their last retirement fence; failed/unsubmitted packets and the terminal completion keep zero tokens. All workers finish before unwinding. **Begin is then rejected for this executor**: a partially executed texture plan may not have reached its final state. The caller must Drain and recreate execution/resources instead of silently reusing stale initial-state assumptions. The application propagates the error to shutdown; automatic partial-frame recovery is not implemented. Tests exercise both late error and active cancellation after actual early GPU progress, verify token/fence retention, reject stale reuse and successfully Drain.

Native submission/Signal failure keeps the existing quarantine policy: retain allocator/list references if completion cannot be established. Known successfully signalled partial recording failure can drain normally; it does not take that unknown-completion leak path. External resources must outlive executor drain, including when Execute throws.

## Verification and measured evidence

Hardware: NVIDIA GeForce RTX 5050, logged 7910 MiB dedicated memory, same driver `32.0.16.2114` as U2. Exactly one engine device is initialized. Other adapters are unused. Toolchain remains VS 18 Professional MSBuild 18.9.1, v145, SDK 10.0.26100.0, x64. No toolchain/global settings changed. Startup isolation retains primary=0, secondary=-1, async=false, native output and frame generation Off.

All final commands below passed with exit 0, from repository root. Build helper logs go to `Build/Verification/meta-render-graph-u3/`, with a 600-second build limit; every GPU smoke process has a 60-second limit. Heavy builds and GPU runs were sequential.

```powershell
foreach ($target in @('PlanTests','RecordingJobsTests','DirectExecutionGpuTests','App.MetaRenderGraph')) {
    & Apps/App.MetaRenderGraph/tests/Invoke-Build.ps1 -Configuration Debug -Target $target
}
& Apps/App.MetaRenderGraph/tests/Invoke-Build.ps1 -Configuration Release -Target App.MetaRenderGraph
& Build/bin/x64/Debug/PlanTests/PlanTests.exe
& Build/bin/x64/Debug/RecordingJobsTests/RecordingJobsTests.exe
& Apps/App.MetaRenderGraph/tests/Invoke-Smoke.ps1 -Configuration Debug -Target DirectExecutionGpuTests
& Apps/App.MetaRenderGraph/tests/Invoke-Smoke.ps1 -Configuration Debug -Target App.MetaRenderGraph
& Apps/App.MetaRenderGraph/tests/Invoke-Smoke.ps1 -Configuration Release -Target App.MetaRenderGraph
```

CPU coverage: 0/1/37 buckets; intentional varied completion delays; deterministic indexed results; nested parent/children/continuation; one/four scheduler threads; serial/parallel; thrown recording error; cancellation before work and during a running child; shutdown/destructor join without explicit Wait. Planner tests retain U2 coverage and add per-submission COMMON transitions plus invalid decaying final-state rejection.

GPU coverage: single-list operations, two upload buckets and a second independent group with four buckets writing disjoint ranges of one destination; exact readbacks for the independent buffer and the existing transform/diamond/draw; serial/parallel pairs with equal seed; prefix/suffix sentinel; repeated two-slot frames; pre-submit error; late error/cancellation after an early submission; 150 ms native queue gate proving Begin waits before reuse; rewrite of the same descriptor/upload range and another successful frame. Final observed Begin waits were 151 ms in all three final runs. All runs also present eight application frames, resize, close and drain with zero D3D12 ERROR/CORRUPTION messages and zero legacy graph Execute calls. Hostile experimental INI is restored and production INI hashes are unchanged.

CPU traces are real native recording spans: after a rendezvous, each of two workers performs 16,384 CopyBufferRegion recordings on its group's own destination. Repeated copies preserve the expected bytes. This deliberately amplified fixture workload makes concurrency measurable; it is not representative scene timing. Runtime intersection below measures only that native recording loop, excluding the rendezvous. Chrome JSON spans additionally include callback setup/Close. `submit packet 0` is recorded on owner tid 0 in the same clock.

| Final run | Recording workers | Native recording intersection | Packet 0 Signal marker | Late independent bucket Close/span end |
|---|---|---:|---:|---:|
| Debug DirectExecutionGpuTests | 2 and 3 | 12,443 us | 15,567 us | 20,319 us |
| Debug App.MetaRenderGraph | 2 and 3 | 13,445 us | 16,300 us | 21,543 us |
| Release App.MetaRenderGraph (clean rebuild) | 2 and 3 | 11,125 us | 13,585 us | 18,215 us |

In each run the late independent bucket cannot finish until the early packet's fence has **actually completed on GPU**. A global recording join would time out. Thus timing evidence includes both real simultaneous CPU command recording and early submission before that late list is closed. It proves neither GPU overlap nor an FPS benefit. CPU traces are not PIX captures.

Initial sandbox build stalled without producing useful diagnostics and its own process tree was stopped before native retry; an overlapping retry failed on the held log. Native builds succeeded. One intermediate C++ build caught a ComPtr/pointer type mismatch, corrected before testing. The old Release compiler PDB generated LNK4020 warnings; a clean rebuild of only the experimental Release project regenerated it, then Release smoke passed again. Exact cleanup/rebuild command (exit 0):

```powershell
& 'C:/Program Files/Microsoft Visual Studio/18/Professional/MSBuild/Current/Bin/MSBuild.exe' Apps/App.MetaRenderGraph/App.MetaRenderGraph.vcxproj /t:Rebuild /p:BuildProjectReferences=false /p:Configuration=Release /p:Platform=x64 /m:2 /nr:false /v:quiet /fl /flp:logfile=Build/Verification/meta-render-graph-u3/App.MetaRenderGraph-Release-rebuild.log
```

Final clean Release log has zero LNK4020 warnings. Existing third-party LNK4099 missing-PDB warnings remain. `git diff --check` passes. App/Base/Engine sources were not changed, so the production App and shared regression suites were not rebuilt/repeated beyond U2's accepted verification. No full Bistro runtime, scene image A/B, PIX capture or performance benchmark was performed.

## Evidence and handoff limits

Local evidence under ignored `Build/Verification/meta-render-graph-u3/`:

- `*-build.log`, `*-stdout.log`, `*-stderr.log`, the clean Release rebuild log and CPU `*-runtime.log`.
- `Debug-DirectExecutionGpuTests/`, `Debug-App.MetaRenderGraph/`, `Release-App.MetaRenderGraph/`: `runtime.log`, `recording-cpu-trace.json`, `direct-schedule.txt`, `reference.dot` (text schedule), effective INI and protected configuration hashes.
- `trace-summary.json`: parsed workers/intervals/early-submission markers; `handoff.json` and `base-to-u3.patch` include the final source/artifact manifest.

The new path still executes synthetic object-update-style buffer fixtures, not migrated geometry/object culling or Bistro techniques (those begin in U4). There is no CPU draw-loop replacement for ExecuteIndirect. Native resources/ranges are fixed before recording; no dynamic DAG mutation during recording, general transient descriptor allocator, aliasing, GPU async, multi-device runtime or automatic error recovery is claimed. Submit order is deterministic, not opportunistically reordered around a delayed earlier packet. Trace/task allocation and extra boundary-list overhead are explicit experimental costs.

Next authorized unit would be U4, `04-geometry-shadows.md`, **only after originating-chat review and user authorization**. U4 was not started.

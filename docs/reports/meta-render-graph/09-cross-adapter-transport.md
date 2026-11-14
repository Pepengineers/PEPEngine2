# Cross-adapter transport — source implementation

## Water-stage prerequisite correction — source only

The subsequent water integration pins `LastSubmittedDisplay` in this fixture.
After Strict queued a newer pending import and its DIRECT readers, switching to
Latest cannot select an older Current and regress the payload. Latest reuses the
last submitted display; preceding DIRECT FIFO already contains its import wait,
so the repeated consumer adds no future wait. The pin is included in FreeDisplay
retirement. This narrow source correction has no build/test/runtime acceptance;
all original hardware gates remain pending.

Date: 2026-11-14 (Europe/Moscow). Checkout: `D:\Project\PEPEngine2`.
Branch: `codex/meta-render-graph-u9`.
Base: **local** `codex/bistro-rendering` at
`19fdc78a00d837c50595d99bb7db7b3dfe7b0349`, the ordinary merge of reviewed
`ee20d6f8dd47e9cc92402a03397125c9cc6643d2`. No remote Bistro reset/rebase.
Result: the commit containing this report; resolve with
`git log -1 --format=%H -- docs/reports/meta-render-graph/09-cross-adapter-transport.md`.

**Implemented in source only; unbuilt/unrun, hardware acceptance pending.**
The user deferred every intermediate runtime gate and explicitly prohibited
builds, compilation, tests, applications, validation/evidence scripts and GPU
runs during this implementation. No hardware identity, bytes, debug-layer
result, CPU/GPU overlap, timing, driver memory or performance measurement is
claimed. The adapters described below are configuration/ownership contracts,
not a report of devices opened in this chat.

## Complete source coverage

| Plan boundary | Implementation and ownership |
|---|---|
| Two explicit physical adapters | `CrossAdapterTransfer::Impl` resolves the existing primary by its actual LUID and the explicit secondary by `IDXGIFactory6::EnumAdapters1`. Both must be hardware; equal LUIDs, missing index and unsupported sharing fail explicitly. Only contexts 0/1 exist; no third device or single-device fallback. Diagnostics log both LUIDs, secondary index, domain incarnation and fixed payload size when a future authorized host initializes. |
| Device-aware resources/queues | `Graph`/`Schedule::Device` scope resource IDs and all local packets. `CompletionToken` carries device, queue, value and domain incarnation. `QueueSubmission` checks canonical COM resource/list owner identity, physical LUID, live native device incarnation and removal of either device. Each shared allocation has distinct resource handles on its two devices. |
| Common producer/export/import/consumer path | `Executor` is the recording context; `QueueSubmission` is the single shared owner of Wait/Execute/Signal transactions and per-queue issued values. Both the original primary scene and the new contexts use it. Secondary COMPUTE records the deterministic produce/pack dispatch; secondary COPY exports the full buffer; an executor-owned cross-adapter fence publishes readiness; primary COPY imports; two actual DIRECT packets copy the selected display into two primary frame-slot result buffers. There is no transport-owned native submission or legacy offload worker. |
| Bounded publication and three lifetimes | Three packed buffers, three shared allocations and three display buffers are allocated before Start. Packed retires at export completion (or COMPUTE completion for an unpublished step); shared retires at primary import completion; display retires after every submitted reader, plus its import and CPU lease. Current and pending display remain pinned independently. Metadata, size, sequence, generation and bytes belong to the same publication. |
| Strict and LatestCompleted | Strict chooses the newest **issued** export, even if not completed, and passes its real token through import to the first DIRECT reader. Host API optionally requests an exact sequence; an unavailable/unissued sequence returns zero, never a future fence promise. Latest selects only completed exports, starts a separate import and retains the previous completed display until the new import completes. Same bounds, leases and retirement rules apply to both. |
| Independent loops and recording | Producer has an OS owner thread, its own fixed recording contexts, immutable per-step seed/header captures and a separate JobSystem using the existing nested Recorder. It submits while primary Present is absent. With a full publication ring it continues deterministic COMPUTE steps in safely retired packed buffers without export. Busy packed/recording slots cause nonblocking polling/condition-variable backpressure on the owner thread, never a pool-worker GPU wait. Primary and secondary native recording jobs can run concurrently. |
| Stop/errors/device removal | No ready publication is committed until export submission and its signals succeed. Cancellation/recording failure publishes no failed packet and no invented completion. Explicit stop joins recording and drains only issued work; abandoned Writing reservations are cleared only after safe producer drain. Stop checks domain health even with zero saved tokens. Any unknown native completion or removal quarantines the entire two-device domain; failed transport teardown retains its complete Impl and all buffers, heaps, devices and contexts. A display lease missing retirement evidence also fails closed. |
| Real experimental host integration | `MetaRenderPipeline` constructs/starts transport only under startup opt-in, shares its submission owner with the existing primary scene, appends/records both DIRECT readers and retires successful or partially submitted reader frames. World drain pauses producer and safely resumes it on the next Append; fixed transport resources survive scene resize. Startup/verify mode conflicts fail explicitly. Production App remains unchanged. |

## Exact publication and queue boundaries

The payload is **1056 bytes**: a 32-byte header and 256 uint32 values. Header
layout is sequence:uint64, generation:uint64, incarnation:uint64,
total-buffer-bytes:uint32, seed:uint32. One COMPUTE dispatch writes both header
and data into the chosen packed buffer. Word `i` is
`(seed + i * 1664525u) ^ sequence.low ^ (sequence.high * 1013904223u)` with
uint32 arithmetic. Layout static assertions and input/bounds/generation checks
are source checks; the embedded shader has not been compiled here.

After export Execute/Signal succeeds, the producer atomically publishes its
immutable metadata and issued Ready token under the slot mutex. Ready means
CPU-submitted, not GPU-completed. Import copies the entire bounded buffer into
one free display slot, and retains its metadata with its import token. Shared
reuse requires that import token to complete. Display reuse additionally
requires all queue reader tails to complete, no outstanding CPU lease, and that
it be neither Current nor Pending. The two fixture readers are both DIRECT;
their greatest issued DIRECT token retires both by FIFO ordering. Failed frames
retain any successfully issued reader token but do not advance
`DisplayedSequence()`. That accessor reports the version of the last fully
submitted consumer frame, including Strict's pending display; it does not
claim GPU completion or Present success.

Old unclaimed exports can be dropped only after their Ready token completes,
when a newer selected publication supersedes them. Claimed/importing slots are
never dropped. A full shared ring does not overwrite any slot, allocate another
buffer or stop independent computation. Submitted step sequences can therefore
have gaps between publications. Exact Strict requests can return unavailable
for a step that was computed without publication or was already superseded.

Secondary and import schedules have `FrameBoundary=false` and terminal COPY.
Their local terminals join their own packets and retire their allocators;
**they are not packets/dependencies of the primary frame terminal**. The
independent contexts do not acquire U8's previous-DIRECT-frame dependency.
Latest's primary DIRECT reads only a completed display import, so it gains no
wait on a future producer or on the new in-flight import. Strict's import can
wait on an already-issued producer signal, and only its selected DIRECT reader
waits for that import. Independent scene packets precede that late reader wait.
Changing selection mode cannot cancel waits already submitted by an earlier
Strict frame.

The primary scene retains U8's conservative whole-resource reader ordering,
previous-frame boundary and all-packet DIRECT terminal within its own schedule.
Transport uses DEFAULT buffers with per-Execute COMMON decay; compiler-owned
transitions and packet tokens handle COMPUTE/COPY/DIRECT handoffs. UPLOAD stays
GENERIC_READ and READBACK stays COPY_DEST, including non-DIRECT terminal
contexts; fixed heaps receive no terminal transitions or graphics-state reject.
Native waits reference only validated already-issued tokens, which also makes
cross-iteration dependencies acyclic by submission order. Remote fence handles
are created/opened by QueueSubmission, never by technique record callbacks.

## Host API and configuration

Only `Apps/App.MetaRenderGraph/Configs/Startup.ini` receives new defaults:

```ini
Meta.CrossAdapter.Enabled = false
Meta.CrossAdapter.SecondaryAdapter = -1
Meta.CrossAdapter.Selection = 1
Meta.CrossAdapter.Seed = 1
```

For a later authorized ordinary Meta host run, enable the startup option and
set the secondary index explicitly, distinct from the adapter actually chosen
by existing `Render.PrimaryAdapter`. Selection 0 is Strict; 1 is LatestCompleted.
Invalid selection/index, WARP/same device and unsupported cross-adapter sharing
produce errors. Enabled/index are startup-only. Seed changes are copied into
the next producer's immutable inputs; producer advancement has no Present tick.
Single-GPU smoke/A-B verification and this opt-in cannot run together.

`CrossAdapterTransfer` provides Start/Stop, thread-safe SetSeed, Append's
selected sequence (zero if unavailable), Record, Waits, RetireFrame, submitted
step and displayed-sequence diagnostics, and the two ConsumerResult buffers.
The host calls Append during primary plan assembly, passes Waits to the existing
Executor, dispatches Record callbacks and passes Completions to RetireFrame on
both success and partial failure. The transfer and primary device must outlive
all recording contexts using its shared submission owner; pipeline member
order enforces that. The result buffers are fixture outputs protected by the
primary frame slot, not a new water/particle renderer or a readback oracle.
Actual visualization and exact readback comparison remain acceptance work.

The low-level sharing model follows the existing WaterOffload reference, but
its worker/submissions are not started. Existing SceneWaterParticles continues
to force local water execution and particle BeginFrame(localOnly=true).
No production solver, water/particle placement policy or production CVar changes.
The two CPU-only fixture project exclusions were updated for the new GPU-only
source files; no test scenario or executable was added.

## Static review, verification and remaining limits

Read the complete meta and transport plans, async report and technique inventory;
current compiler/recorder/executor, primary pipeline, inherited project/caller
contracts; WaterOffload sharing/lifetime source and particle offload contracts.
Existing PlanTests and DirectExecutionGpuTests were inspected for dependency,
heap, prefix/suffix, cancellation, partial-submit and retirement contracts.
No test source changed. The deliberate verification exception is the user's
explicit prohibition: source assertions/input checks, source/diff review and
`git diff --check --ignore-submodules=all` replace runtime checks for this handoff.

Source review covered both selection branches, late waits, native and canonical
device identity, physically distinct handles, single submission ownership,
COMMON decay/fixed heaps, three lifetimes, both readers, full rings, no-Present
advancement, partial submissions, stop before first token, removed secondary
behind a primary wait, world-drain cancellation reservations and restart.
Quarantine checks apply to all queues/devices, not just the failing queue; a
removed remote device cannot be ignored by a healthy local drain. Canceled
recording cannot publish a token that will never be signalled. Existing owner
SDK/post/UI/particle recording remains sequential; native scene workers retain
their U8 path. Independent contexts are source-connected, not measured overlap.

Transport buffer count is fixed at 3 packed + 3 shared allocations (two local
handles each) + 3 display + 2 results per primary frame slot. Each new recording
context reserves 6 lists/allocators per queue type per slot: 54 pairs for the
secondary context and 54 for import. Resource/heap alignment and driver memory
are hardware-dependent and unmeasured. CPU recording plan objects are bounded
by the fixed packet counts, not an accumulating work queue. Failure quarantine
deliberately retains the bounded device domain until process exit; there is no
automatic device recovery or arbitrary technique migration.

`git diff --check --ignore-submodules=all`: passed for the reviewed source diff.
The unrelated `Apps/App/tests/BistroAdditionsTests.cpp` remains unstaged and
untouched with SHA256
`BAF2D4E7B6ED94E634DA4821EFACD62743AF55FBEE86A81504C9AC6665F84EE4`.
Submodules/inih metadata were not traversed or modified.

**NOT RUN:** Debug/Release builds; any C++/HLSL/shader compilation; CPU/planner,
unit/integration/regression/GPU tests; smoke/validation/evidence scripts;
App/App.MetaRenderGraph or any application; debug layer/GBV; GPU readbacks,
images/visual checks; benchmarks, timing captures or profiling. Historical
binaries/captures validate none of these changes. Hardware acceptance still
requires two actual cards, exact payload/header checks over ring rotations,
both version modes and reader lifetimes, independent Present pauses, failures,
world drain/resize, debug-layer evidence and a real two-device queue timeline.

This handoff stops after source implementation and the requested branch push.
No PR, Bistro merge, U10 implementation or hardware acceptance was performed.
The originating chat owns independent review and any subsequent local merge.

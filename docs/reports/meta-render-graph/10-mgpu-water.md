# Water through the two-device executor — source implementation

Date: 2026-11-14 (Europe/Moscow). Checkout: `D:\Project\PEPEngine2`.
Branch: `codex/meta-render-graph-u10`.
Base: **local** `75cb0cf35c3229d398b400a0f7bc2c199baeae86`, reviewed transport
merge on `codex/bistro-rendering`. No remote reset/rebase/pull.
Result: the commit containing this report; resolve with
`git log -1 --format=%H -- docs/reports/meta-render-graph/10-mgpu-water.md`.

**Implemented in source only; unbuilt/unrun; hardware/runtime acceptance pending.**
This handoff does not establish numerical/image correctness, GPU overlap or
performance. The user explicitly prohibited builds, compilation, tests, new test
scenarios, applications, evidence scripts and GPU workloads in this phase.

## Connected source path

Startup opt-in defaults are `Meta.Water.Enabled=false`,
`Meta.Water.SecondaryAdapter=-1`, `Meta.Water.Selection=1`. The adapter is an
explicit DXGI EnumAdapters1 hardware index, distinct from the actual primary
LUID. Missing/same/software adapters and unsupported sharing fail explicitly.
Selection 0 is Strict; 1 is LatestCompleted. Water excludes the deterministic
CrossAdapter fixture and single-GPU A/B verification. Production App defaults,
callers and existing offload remain unchanged.

`Techniques/WaterTransfer.*` owns one secondary device and one independent
producer thread for all collected nonempty water instances. Its contexts and
the primary scene share the existing `QueueSubmission` domain and `Executor`.
It never constructs/starts `GDX12WaterOffload`, invokes the old graph Execute,
or sends private native queue work. Only the common submission service owns
Wait/Execute/Signal. Solver state/constants/descriptors stay secondary; the
production `MakeSeed/PlanSteps/Record`, settings and numerical kernels are reused.

| Packet | Placement and lifetime |
|---|---|
| Seed/SPH | Secondary COMPUTE; immutable inputs, original ping-pong/density/keys/offsets |
| Pack | Secondary COMPUTE; original Pack writes float3 positions to the chosen packed slot |
| Export | Separate secondary COPY; all instance regions enter one shared allocation; packed retires at export completion |
| Import | Standalone primary COPY; shared regions enter corresponding received buffers; shared retires at import completion |
| Display seed/history | Late primary DIRECT; previous displayed positions, then selected positions, or history-only repeat |
| Surface/resolve | Original primary DIRECT sphere/thickness/NRF/merge/optics, far/near transparency and downstream depth/motion |

Producer/import graphs have `FrameBoundary=false` and terminal COPY. Their
terminals do not join the primary all-packet frame terminal. Only the selected
history packet waits on its issued import token; independent primary scene work
can precede that late wait. Local particle A/B pools, depth-dependent B on primary
COMPUTE, one combined SortedAlpha and WBOIT remain connected. No particle offload.

## Inputs, payload and progression

CPU publication metadata retains epoch, generation, device incarnation, bytes,
publication sequence, and per-instance ID, seed/reset identity, full immutable
settings/transform, ClockScale, actual wall-derived DeltaSeconds, Batch.Count /
Batch.Dt and FirstStep / LastStep. The step interval is exclusive at FirstStep;
LastStep minus FirstStep equals actual dispatched substeps. Solver step counters
advance by Batch.Count; publication sequence identifies a batch and can have gaps.
Metadata is separate from the shared payload: exactly sum(count * sizeof(float3)).
Velocities, scratch, density, keys, offsets and solver PreviousPosition stay
secondary. Surface consumes positions and previous displayed positions, not
solver velocities.

The producer snapshots mutable inputs under its mutex, then validates publications
against fixed topology identity. It uses its own steady clock, applies ClockScale
once and TimeScale once, and preserves PlanSteps policy. Pause/disabled/zero scale
permit initial seed but no integration. Present does not tick the producer;
pausing primary Present leaves an already running unpaused solver advancing.
Its own Executor/JobSystem/contexts use TryBegin and completed tokens; there is no
mandatory CPUWait per batch. A full shared ring can skip publication while
solve/pack continue in safely retired packed slots. Busy slots use bounded polling
backpressure, without allocating another GPU slot or accumulating work.

There are three packed versions per instance, three shared allocations for the
topology, three received versions per instance and one persistent display per
instance. These lifetimes are separate. Received reuse requires import and last
history-read completion, no CPU lease, and exclusion from Current/Pending.
Display writes and every surface reader share primary DIRECT FIFO. Frame
retirement preserves the highest actually issued tail per primary queue across
frames, including empty-water frames and partial failures.

## Selection, history and lifecycle

Latest imports completed exports separately and retains the display until a new
import completes; it adds no future solver/import wait. Strict can select an
issued export/import and waits only on its real tokens. A selected-display
watermark prevents Strict-to-Latest from overwriting newer submitted positions
with older Current data. Prior Strict waits remain in DIRECT FIFO order.

The first history packet seeds primary displays even before publication. Every
displayed frame updates PreviousPosition, including repeated display. The small
`RepeatDisplay` shader entry advances history without binding/reading Received
when none is selected, including retained displays after topology changes.
Original Pack/AdvanceDisplay and numerical kernels are unchanged. Surface retains
its previous primary-frame container transform, camera history, ResetMotion and
first-render controls. `SetExternalDisplay` skips local solver allocation and
legacy offload startup, keeps constants/render controls and declares caller-owned
display buffers. App-owned water was not moved into generic Engine code.

Cold topology/source/reset changes stop on a complete bounded batch, join, and
drain producer/import/primary reader tokens before replacing resources. Unchanged
instances retain solver head, wave clock, actual steps, seed and display. Reset
of one water does not restart other solvers or particle pools. World-history reset
explicitly recreates all technique state after all retained frame tails drain.
Resize drains primary execution before replacing surface/particle targets;
solver/transfer state is independent of viewport extent. Healthy Stop/world drain
resumes on the next Prepare without resetting the solver or stranding Writing
reservations. Seed uploads remain retained; constants reuse requires the selected
packed/context slot to complete.

Stop does not cancel Execute midway through a solver batch. A started batch
finishes submission and CPU head/step commit before exit; real failures are saved
even during Stop. Producer context drains/destructs on its owner before secondary
device destruction. Unknown completion/removal quarantines the common domain;
failed teardown retains the complete water Impl, buffers/heaps/devices and import
context. Missing frame lease retirement also fails closed. Writing reservations
clear only after join and successful drain. Claimed/occupied storage cannot be
reset before completion evidence permits reuse/destruction. Recording/submission
failures require the existing execution/resource recreation procedure; there is
no automatic device recovery.

The U9 fixture received the analogous mode-switch correction: pin the last
successfully submitted display, reuse it rather than regress to older Current,
and omit a new import wait when preceding DIRECT readers already queued it.
That source correction is not runtime acceptance of the fixture.

## Source checks and limits

Read complete meta/water/transport/local-water plans, transport/local-water reports
and inventory; actual solver/collector/offload/shaders/surface/resolve, scene
adapter, compiler/executor/submission and host lifecycle/callers. Existing
PlanTests, DirectExecutionGpuTests, FluidSimulationChecks, WaterCompositionChecks,
WaterOffloadGpuTests and WaterOffloadBistroTests supplied heap, partial-token,
time/settings/reset and history contracts. No test scenario was added/changed.
CPU-only project exclusions were minimally updated for the GPU source and caller.

Source review covered selection/mode switching, initial/repeated history, actual
step snapshots, multiple instances/isolated reset, empty topology, ring saturation,
packed export readers, received history readers, DIRECT display FIFO, DEFAULT
COMMON decay/fixed UPLOAD state, partial tokens, stop/restart, descriptor capacity
and quarantine. `git diff --check --ignore-submodules=all` passed. The unrelated
unstaged `Apps/App/tests/BistroAdditionsTests.cpp` remains untouched with SHA256
`BAF2D4E7B6ED94E634DA4821EFACD62743AF55FBEE86A81504C9AC6665F84EE4`.
No submodule was changed or traversed through inaccessible metadata.

The fixed secondary texture descriptor range requires 16 descriptors/instance,
allowing at most 24 instances; larger topology fails explicitly. Primary uses the
existing shared descriptor allocator and can fail on exhaustion. Memory includes
original solver/scratch, three packs, three shared allocations, three receives,
display and retained seed plus three constants per instance. Producer reserves
9 list/allocator pairs per queue/slot (81 total), import 6 (54 total). Driver
alignment/residency/peak bytes are unmeasured. Solve/pack share COMPUTE FIFO;
different packed slots permit export overlap in source, without hardware evidence.
Cold topology/world changes deliberately wait. CPU metadata/graph allocations are
bounded by fixed topology/packet counts, not claimed allocation-free.

**NOT RUN:** tests/builds, C++/HLSL/shader compilation, applications,
smoke/validation/evidence scripts, CPU/GPU/readbacks/images/visual checks, debug
layer/GBV, benchmarks or profiling. Hardware acceptance remains pending: frozen
real-step numerical comparison, exact bytes over ring rotations, multiple
instances/Present stalls, both selection modes/switching, displayed motion and
container/camera, pause/timeScale/reset/resize/shutdown/errors and device timelines.

Stop for originating-chat independent review and any authorized local merge.
No push, GitHub PR, Bistro merge or next-stage implementation was performed.

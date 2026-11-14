# Async queues on one GPU — source implementation

Date: 2026-11-14 (Europe/Moscow). Checkout: `D:\Project\PEPEngine2`.
Branch: `codex/meta-render-graph-u8`.
Base: local `codex/bistro-rendering` at
`c444d58a30b0921c2d126dbf252ce116a0e6d196`, containing the ordinary merge of
`4181d0e16687fb3d44c59c781412751dd1374268` (post/UI, provider rebinding and
retained FG states). No reset to the older remote Bistro branch was performed.
Result: the commit containing this report; resolve with
`git log -1 --format=%H -- docs/reports/meta-render-graph/08-async-single-gpu.md`.

**Implemented in source only. Unbuilt, unrun; no runtime acceptance.** The user
explicitly deferred every build/test/application/GPU check and permitted this
implementation after originating-chat source review of its prerequisite.
Production App selection is unchanged. There is one device, no cross-adapter
transport, secondary worker or next-stage implementation.

## Queue placement and complete source coverage

| Plan step | Concrete implementation |
|---|---|
| Queue identity and producer chain | `Operation::Target` / `Packet::Target` select actual DIRECT/COMPUTE/COPY queues, lists and allocators. The existing DirectFixture opt-in plan records independent DIRECT clear → COPY upload → COMPUTE transform → DIRECT draw; default callers remain DIRECT. Scene palette uploads run COPY, skin deformation COMPUTE, geometry consumers DIRECT. Scene/material/transform/main/camera upload buckets run COPY, visible-count reset and scene culling COMPUTE, depth/GBuffer ExecuteIndirect DIRECT. These are existing algorithms and real scene buffers, rather than labelled DIRECT lists. The owner prefix and DIRECT poster prelude precede the first dependent DIRECT wait. |
| Fork/join and state ownership | Compiler keeps RAW/WAW and the complete reader set for WAR, explicit dependencies, side-effect order and cycle rejection. Cross-queue whole-resource state ownership adds dependencies and producer release to COMMON before consumer transitions. Initial imported states illegal on COMPUTE/COPY get a separate DIRECT release token. Terminal DIRECT joins every packet. Explicit read-state unions remain intact where queue-legal; changing a union or moving a DEFAULT read between queues is conservatively serialized, rather than assuming concurrent incompatible states. |
| Real mixed simulation | Particle input group A (depth independent) stays DIRECT; B (depth collision) runs COMPUTE on the same device. Initialization, simulation and access declarations filter physically separate per-emitter pools. B reads the actual opaque-depth producer and interaction field. A retains valid camera/depth descriptor bindings and original dimensions, but neither samples nor transitions scene depth. Existing seeds, pause/reset, bursts and numerical kernels are reused. |
| Decomposed consumer | DIRECT clear/cull A reads A pools plus its own render scratch/constants, before the B wait. It declares neither B pools nor raster depth/atlases, with water on or off. B is appended after independent mesh transparency work; only then are draw arguments finalized and ONE global SortedAlpha radix sort run. Weighted/feature-motion and sorted/additive drawing still consume that combined result. The existing near WBOIT accumulation is resolved once, never separately by queue. Draws are deliberately after the global join; the independently available work is A preparation. |
| Uploads, early submission and reuse | Palette and four frozen scene-upload buckets use COPY. Existing unrelated uploads/SDK internals remain in their original owner sections. Recorder workers still prepare independent lists; owner-only bodies are recorded just before their packet, so they no longer impose an all-owner/all-frame recording barrier. Each ready topological packet submits without joining later packet recording. Selected-slot fences for ALL queues are waited before material/poster/scene preparation. Allocators and frozen uploads/descriptors remain slot-owned. |

`Meta.Async.Queues=false` explicitly compiles the scene onto DIRECT as the
serialized placement reference; true selects the mixed placement above. This
variable belongs to App.MetaRenderGraph only. Existing geometry-only fixture
callers retain single-DIRECT placement by default. The inherited diagnostic
comparison selects single-DIRECT for its reference and the configured placement
for its displayed recording, but still compares geometry/lighting readbacks
only; it is not a new particle/post/temporal acceptance oracle.

## Barriers, tokens and retirement

The CPU-only graph now declares `Heap::Default`, `Upload` or `Readback`, exported
in the compiled schedule and verified against each physical D3D12 heap by the
executor. UPLOAD remains read-only GENERIC_READ, including COPY source use;
READBACK remains write-only COPY_DEST, including COPY destination use. Neither
gets transition barriers. Equal initial/final states on a DEFAULT texture never
imply a fixed heap. Existing fixture declarations were updated for this contract;
no test scenario was added or run.

DEFAULT buffers decay to COMMON after each packet ExecuteCommandLists. Their
next packet transitions from COMMON, with its dependency fence providing memory
ordering. Ordinary texture states persist; producer epilogues release them to
COMMON for a queue handoff. COPY texture accesses explicitly release to COMMON
before their submission's decay. Within a packet, prologue/body/epilogue lists
share one Execute call; no decay or imaginary full list-boundary barrier is
assumed inside that batch. Explicit same-state UAV ordering remains in the plan
and existing native kernel recorders. COMPUTE rejects graphics-only states and
COPY rejects UAV/non-copy states before submission. CUSTOM heaps and
simultaneous-access textures remain rejected, rather than guessed.

Packet dependencies are packet-index completion tokens resolved to the producer
queue's actual issued fence value. The one owner submits Wait/Execute/Signal
transactions. A queue waits only already-signalled producer tokens immediately
before the dependent packet; there are no forward native waits on unrecorded or
unsent packets. Prefix/suffix remain DIRECT. Schedule dumps identify each queue,
wait token, signal token and both release/acquire barriers.

Each slot records its last successful DIRECT, COMPUTE and COPY fence. Begin now
runs in `PrepareMaterialBindings`, before poster/material/skeletal/scene owner
preparation, rather than late in ExecuteFrame. RenderModule's earlier main-CB
rewrite already waits the returned DIRECT frame completion; the terminal DIRECT
packet joins all other queues. This protects owner inputs both before and inside
ExecuteFrame. No unconditional all-slot CPU drain was added to ordinary frames.

Persistent simulation/scratch resources span frame slots. The first COMPUTE and
COPY submission of each new plan additionally waits, on GPU, the previous
successful DIRECT terminal/suffix. This protects previous DIRECT readers even
when Execute has no prefix. It is a conservative frame-to-frame dependency, not
an independent simulation clock. DIRECT retains natural queue ordering. Reuse
of a selected slot also waits all three recorded fences independently, including
when a later recording failure prevented terminal submission.

Drain waits only executor-issued submitted work. The optional `DrainExecution`
pipeline seam brings its queues into RenderModule teardown/world drain before
resource release; legacy pipelines keep their existing behavior. Meta resize
also drains before replacing its post resources, and RebuildExecution drains
before geometry/environment/water replacement. Existing cold allocations and
SDK/swapchain presentation can still use their original owner drains/flushes;
there is no newly introduced global Flush per ordinary frame. Frame generation
retained NON_PIXEL_SHADER_RESOURCE exports and provider per-slot rebinding are
preserved. SDK/post/UI and water internals remain explicitly owner-only CPU
sections, and water stays DIRECT.

## Errors and boundaries

Compiler rejects cycles and unsupported queue states before any submit. Executor
checks heap/state contracts, list identities, topological dependencies and the
all-packet DIRECT retirement join before recording. Worker failure/cancellation
joins all jobs and does not publish the failed packet. Earlier successful tokens
and per-queue fences are retained; no submitted queue can await a future token
that cancellation removes. A partial frame rejects further Begin until executor
and resources are drained/recreated. There is no automatic state-history repair.
Unknown native Execute/Signal completion quarantines command lists/allocators
and queues; RenderModule's existing GPU-owner quarantine remains authoritative.

Borrowed GDX12CommandList takes its real native list type. Existing command-type
guards continue to reject graphics calls on COMPUTE/COPY and dispatch on COPY.
No callback gains submission ownership or private fence signals. Particle group
recording and mutable pool indices remain owner-only, not falsely advertised as
parallel CPU recording. Geometry/grass/lighting native recording retains its
existing worker path. Ready packets still submit in deterministic topological
order, so a delayed earlier packet can hold independent later submissions.

## Static review and verification limits

Read the complete meta and async plans; post/UI report and inventory; relevant
recording, geometry/grass, lighting and water/particle reports and source; all
changed APIs/callers and existing planner/direct/lighting fixture declarations.
Source review traced palette/scene COPY → COMPUTE → DIRECT consumers, mixed pool
ownership, A preparation without depth/atlas acquisition, common sort and WBOIT,
fixed heaps, imported/retained texture states, packet release ownership, prefixless
cross-frame reuse, owner preparation, cancellation, resize and shutdown drains.
Originating-chat observations about READBACK transitions, broad poster reads and
A's unused depth acquisition/bindings were addressed in this source review.
A combined source simplification pass found one redundant drain alias; it was
removed. No runtime check was used to justify that API-only cleanup.

`git diff --check --ignore-submodules=all` passed. This checks text/diff hygiene
only. Existing `inih` metadata was not traversed or changed. The unrelated
`Apps/App/tests/BistroAdditionsTests.cpp` stays outside the task/commit with SHA256
`BAF2D4E7B6ED94E634DA4821EFACD62743AF55FBEE86A81504C9AC6665F84EE4`.

**NOT RUN:** all Debug/Release builds; C++/HLSL/shader compilation; CPU/planner,
unit/integration/regression/GPU tests; smoke, validation or evidence scripts;
App/App.MetaRenderGraph or any application; debug layer/GBV; GPU readbacks,
images or visual checks; benchmarks, timing captures or profiling. No binary or
capture from an earlier stage validates this source. No runtime correctness,
CPU/GPU overlap, speedup, serial-vs-async timing or memory figure is claimed.

Hardware acceptance remains deferred: queue-chain/diamond/multiple-reader bytes,
late waits and cancellation, multi-slot/prefixless reuse, mixed emitter outputs,
SortedAlpha/WBOIT/motion/history, water enabled/disabled, first frame/reset/resize,
SDK/provider/FG variants, legacy regressions and actual GPU timeline. The list is
pending evidence, not newly added executable tests. List pools now reserve all
three native types per slot (up to three times the old list/allocator count for
the same count reservation); driver memory has not been measured. Conservative
read handoffs and previous-frame joins can reduce overlap. This source handoff
stops here for independent review; no next-stage work or Bistro merge occurred.

# U6 — local water, particles and transparency

Date: 2026-11-14. Base: `a322796512233fc303fed4d54860081ca6a62767`
(U5), descendant of U4 `71f6f4258f26dc4d61ce8800f935a5ce9423d7f0`.
Branch: `codex/meta-render-graph-u6`, host `MidSpec`.

**Реализовано в исходниках, сборки/тесты/приёмка отложены пользователем.**
The user authorized U6 on the final, unbuilt U5 source after originating-chat
static review. Earlier U5.1 binaries/checks provide no U6 evidence. This report
records implementation and static inspection only. Independent originating-chat
review remains pending. U7 is not authorized by this handoff.

## Source path and stages

`SceneWaterParticles` is appended through `SceneLighting::HDRContinuation` after
same-frame SSR/probe production, before diagnostic display. It consumes
`ReflectedHDR`, not the U5 aerial-composited image. The U5 diagnostic composition
and its historical readbacks are retained; the displayed image comes from U6.
No old render graph Execute, private submission, offload worker, COMPUTE queue,
COPY queue or secondary device is introduced in the U6 path.

| Packet | Original implementation and declared inputs/outputs |
|---|---|
| Water seed/SPH | App-owned `GDX12FluidSolver::Record`: seed copy, previous positions, hash/bitonic sort, offsets, density/integration, original substeps, ping-pong and wave clock. Private particles/density/keys/offsets and constants are declared. |
| Water surface | Original sphere/thickness raster, alternating NRF and cleanup, merge. Reads opaque depth/motion and solver particles; writes combined water-or-opaque depth/motion, surface geometry/owner and private scratch. |
| Particle initialization | Original pool/reset kernels; particles, alive/dead lists, counters and dispatch args. Explicit packet even when paused. |
| Particle simulation | Original prepare/simulate/death/spawn/publish kernels and indirect dispatch. Reads opaque scene depth/camera, current grass interaction version and immutable emitter controls; updates pools. |
| Particle render data | Original all-emitter culling, draw-argument generation and FidelityFX radix sorting. All eligible/depth-dependent groups share draw data and one global SortedAlpha sort. |
| Far transparency | Original `TransparentPass.hlsl` PSFar and root/PSO/signature, with U4 immutable scene buffers and transparent indirect commands/count. Reads packed U5 shadows, IBL, LUTs and published probe image/metadata. Clears its accumulation/revealage once. |
| Far composition | Original WBOIT composition of ReflectedHDR and far transparency; applies opaque aerial perspective once. |
| Water resolve | Original water optics/refraction/absorption/scattering/probes resolve over far-composed HDR, using combined depth, opaque depth, owners/geometry and environment. Disabled fallback is far-composed HDR. |
| Near transparency | Original PSNear and same immutable U4 geometry, using combined water depth as separator. Clears near accumulation/revealage once. |
| Weighted particles/motion | Original weighted-alpha raster appends to near accumulators; particle feature-motion attachment is cleared once and includes all blend/mesh regions. No per-emitter/group WBOIT clear. |
| Near resolve | Original WBOIT resolve over water HDR; opaque aerial term is disabled here. |
| Sorted/additive | Original global SortedAlpha and additive HDR composition, including Firefly mesh additive. |

The adapter records transparent mesh geometry directly from U4 buffers, rather
than calling legacy transparency Execute with its mutable visibility caches.
Production bindless material textures retain the existing renderer asset lifetime
contract. No new material/grass transparency algorithm is added (grass uses its
existing opaque/depth/shadow/probe stages).

## Recording, state and ownership

U6 is **owner-recorded**, explicitly marked by `Operation::OwnerRecording`.
Pool heads, burst consumption, water previous transforms and original helpers
still mutate technique-private CPU data. This is a KTD5 sequential CPU section,
not a claim of parallel U6 recording or GPU async. U4/U5 native recording retains
its existing worker behavior. Every U6 owner callback completes before recording
workers/submission begin in Executor; no worker changes state or descriptors.

`GDX12CommandList` can borrow an already-open DIRECT list without an allocator.
The executor still closes, submits, signals and retires it. Borrowed-list Reset
is rejected. `GDX12RecordingStates` is a strict thread-local map keyed by physical
resource; undeclared transition/UAV helpers throw. Legacy helper transitions use
this local map, never shared `_currentState`. Each packet restores its declared
boundary states before closing. Texture boundaries use ALL_SHADER_RESOURCE,
DENY_SHADER_RESOURCE scratch uses COMMON, buffers use GENERIC_READ; DEFAULT
buffer decay between submissions remains modeled by the existing compiler.
Graph accesses are conservative whole-resource accesses; scratch and pool render
reads are intentionally overordered. They do not authorize concurrent use of
shared descriptors or splitting a stage onto other queues.

No command recording callback allocates descriptors or submits/waits a queue.
Water pipeline/solver initialization, pool/draw/sort allocations and constants
preparation happen on the owner before workers. Firefly's opt-in local path uses
a retained upload-heap vertex buffer, avoiding the production lazy mesh upload's
private submit/flush; legacy defaults keep their existing default-heap path.
Cold changes can retain the original owner-side retirement flush. There is no
frame-time performance/memory measurement or performance claim.

Each serial/parallel diagnostic path owns independent water state and particle
simulation/render state. Frame-slot constants follow the primary slot retirement;
persistent simulation/scratch outputs are ordered on the single DIRECT queue and
return through the graph's final resource states. The App's simulation pointer is
installed only during preparation/owner recording and restored by RAII on failure.
The adapter never publishes a CPU-ready result as GPU completion. Output pointers
are same-frame graph inputs, subject to resource accesses and Executor's fence.

## Histories, fallback and lifecycle

Original seeds, reset revisions, burst totals, pause/time-scale, analytic-drag and
semi-implicit kernels, depth/box/volume collisions and interaction constants are
preserved. Water inputs are collected by the App-owned Bistro collector, including
world pause/time scale and container transforms. No numerical kernel or HLSL was
changed. Pool initialization remains separate from simulation so reset while
paused follows the original behavior.

Water's depth/motion outputs are passed to particle drawing and near composition,
and exposed as `Depth()` / `Motion()` for later downstream techniques. Particle
simulation retains opaque depth, as in Bistro. `ParticleMotion()` exposes the
feature-motion attachment for the U7 motion merge. Postprocessing/reconstruction
are not added here. Disabled water falls back to opaque depth/motion and the far
composition; disabled particles leave the mesh accumulators and no stale motion
pointer. Empty pools retain original no-op behavior.

Executor is drained before `RebuildExecution` replaces U6 resources on resize.
World reset and diagnostic mode change schedule both private histories for reset;
owner flush precedes destruction. A frame/recording exception requests a fresh
U6 history, without weakening the executor's existing aborted/quarantined policy.
An aborted executor still requires its existing drain/recreation procedure;
resetting U6 alone cannot resume it. Destruction order retains executor drain
before technique resources are released. App-owned water stays in App sources.

## U8/U10/U11 data contract

`InputGroups()` partitions emitter IDs by actual depth dependence. The full
immutable `GDX12ParticleEmitter` snapshots retained in each simulation include
ID/reset revision/burst total, world transform, dt/pause/time scale/seed, simulation
model/substeps, spawn shape/rate/lifetime/velocity, acceleration/wind/drag,
interaction/volume/box/depth collision controls, blend/geometry/atlas/material.
All groups currently run locally on DIRECT regardless of SharedSimulation flags.
Local-only is an opt-in BeginFrame parameter; default production placement remains.
DepthCollision=true must stay primary when later enabling placement.

The current pool `Particle` is 72 bytes: Position/Age, Previous/Lifetime,
Velocity/Rotation, PreviousAge/PreviousRotation/SizeRandom, Seed and Identity.
The current render `ParticleDraw` is 96 bytes: Position/Size, Previous/Rotation,
Color, SoftDistance, TextureIndex, Frame, Columns, Rows, EdgeSamples, ViewDepth,
PreviousSize, PreviousRotation, Age, PreviousAge and Geometry. Blend regions and
alive/count/indirect metadata accompany it. Future payload must preserve these
fields plus emitter ID/reset revision/burst consumption and simulation publication
version. Keep a common all-emitter draw/sort input across data groups. Interaction
transport requires cells, area/resolution/floor and source version; depth and
camera remain local inputs. The water solver particle is 36 bytes (Position, Velocity, PreviousPosition).
Future water publication needs positions, particle count/instance ID, container/settings/reset and simulation sequence/steps, plus
previous *displayed* transform/history, rather than treating a completed CPU step
as GPU-ready. No transport/submission runtime or offload-worker is implemented.

## Static inspection and deferred validation

Performed: host/cwd/base/ancestry checks; scoped status excluding inaccessible
submodule metadata; foreign-file SHA256; source/caller review of water solver,
particle init/simulation/render/sort, U4 geometry buffers, U5 HDR/probe/LUT inputs;
review of declarations, RAII pointer restoration, lifecycle/error paths, project
source registration and `git diff --check`. These are not compilation or tests.
Full `git status` encountered permission denial in existing inih submodule metadata;
`--ignore-submodules=all` was used thereafter. No submodule was modified.

**NOT RUN:** any Debug/Release build or compilation (including shaders), CPU/GPU,
unit/integration/smoke/regression tests, acceptance scripts, applications,
readbacks/visual validation, debug layer, benchmarks, timing capture/profiling.
No U6 binary, runtime image, numerical equivalence result or acceptance exists.
The inherited diagnostic A/B comparison still covers U4/U5 readbacks; it is not a
new U6 numerical oracle. Required future checks include U6 state/image readbacks,
interleaved cross-group sorting, all particle controls/collisions, transparent
layers around water, previous-display motion, multiple queued slots and
resize/reset/disabled/re-enabled behavior, plus production legacy defaults.
Only after originating-chat code review and fresh user authorization may those
checks run. Stop after this source handoff; do not start U7.

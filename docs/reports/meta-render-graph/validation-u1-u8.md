# U1–U8 validation on MisSpec

2026-11-14. Intermediate review checkpoint. **U1–U8 are not independently accepted;
U9/U10 remain prohibited.** No U11, Bistro merge or PR. Push is authorized only for the task branch.

Frozen base: `ee20d6f8dd47e9cc92402a03397125c9cc6643d2` (tree
`03ee597493fb4cdf4c92a994e0cfab9382c61bce`). Branch:
`codex/meta-render-graph-u1-u8-validation`. Worktree:
`C:/Users/MidSpec/.codex/worktrees/meta-render-graph-u1-u8-validation/PEPEngine2`.
Evidence paths below are relative to `Build/Verification/meta-render-graph-u1-u8/`.

## Current scope

The user's latest instruction excludes **all frame generation and all reconstruction
providers except Native/FSR** from required acceptance. Earlier FG/provider results
remain historical evidence; no further excluded-provider runs are authorized by this
checkpoint. Added FG counters, SDK logging hooks and foreground-window manipulation
were removed. Existing presentation safety/lifecycle fixes remain.

The workstation runs this process in disconnected RDP session 2 (`MidSpec`, `Disc`).
Intel SDK diagnostics explicitly reported `0x087A0001` / `DXGI_STATUS_OCCLUDED`.
This explained failed generation-count checks, including the original standalone
FrameGenerationTests. These are excluded results, not evidence of unsupported GPUs.
Ordinary visible-desktop FPS is unmeasured. Disconnected-session renderer timings
would require their own frozen, matched, non-skipping workload protocol; none have
been accepted or substituted for desktop FPS here.

## Ownership and preservation

The original U6 checkout was idle when the atomic claim was created at
`C:/Users/MidSpec/Documents/PEPEngine2/.tmp/meta-render-graph-validation/owner.json`.
Owner chat: `01a1d930-ecb6-7251-99a2-4c4785c3c5a5`, scope U1–U8. Every workload
rereads that claim and checks competing processes. All workloads are serialized.
The historical MGPU lease remains released and untouched.

Original HEAD: `2ee25daede82062a27bcd9d2add2d0b3ffbdb71c`. Its five staged fixes
and untracked `Apps/App/App.vcxproj.user` are preserved. Original staged patch
SHA256: `00925DF035CC4FA0808BC972678CA838BE730E7C13F278C8DB0BD9AD23D931AA`;
unstaged patch was empty. User-file SHA256:
`C06A75B13F855A94D46616796E024C52B499F8F92CF00CCB571DDBC6FF574676`.
No reset/clean/stash was used. Submodules and 614 cached LFS payloads were hydrated
in the isolated worktree. Only task-owned diagnostic edits were reverted.

## Evidence and coverage

One RTX 5050 GPU/device is initialized. VS18 Professional/v145, x64. Builds use
`/m:1 /nr:false`. Runtime JSON fingerprints each executable; source snapshots store
the tracked diff and hashes/copies of untracked test sources. Effective startup INI
is saved; runtime FSR changes are enumerated in `fsr-checks.csv`.

Completed checks are reused. `--scene-smoke` skips the previously completed isolated
fixtures. `--ao-ssr-matrix --matrix-start=N` resumes at the first unfinished case.
`--fsr-check` checks FSR only. The final handoff manifest identifies exact source
commits, binaries, run labels and hashes; older binaries are not final-source proof.

| Scope | Evidence | Result / actual oracle |
|---|---|---|
| CPU compiler | `candidate-plan-debug-run01` | PASS RAW/WAR/WAW/all readers/UAV/cycles/decay, queue legality, COPY reuse after DIRECT+COMPUTE readers, fixed upload/readback heaps and imported release states. |
| CPU recording | `candidate-recording-debug-run01` | PASS 0/1/37 buckets, nested continuations, serial/parallel, one/four threads, error/cancel/shutdown. Not repeated. |
| Actual queue execution | `candidate-executor-focused01` | PASS COPY/COMPUTE/DIRECT fork/join and two slots, exact pixel/buffers, early DIRECT completion before late COPY recording, late failure/cancellation after progress, poisoned-executor rejection and drain. Async slot reuse waited 154 ms. |
| Focused GBV | `candidate-executor-gbv01` | PASS: errors=0, warnings=35, informational=1. No timing claim. |
| Local water numerics | `candidate-water-debug-smoke01` | PASS original independent numerical checks through Meta executor, 321 states: actual solver, DIRECT/COMPUTE, pause/time scale/reset, pressure/cohesion/prediction/collision, real substeps, reference model and periodic waves. |
| Local particles | `candidate-particles-debug-smoke01` | PASS 99 frames, controls/birth/death/burst/reset/pause/volume/depth/box/friction; displacement tolerance 0.03 cm. Mixed DIRECT/COMPUTE four-emitter SortedAlpha/WBOIT image tolerance 0.003; expected RGB (0.5625,0.3125,0.125), revealage 1/16. |
| Combined geometry/water/particles | `candidate-integrated-scene04` | PASS 8 serial-DIRECT/parallel-async pairs, actual resize, 4 queued frames and shutdown. Original skin/geometry/shadow/grass/probe readback oracles pass. Layer errors/corruption/discarded=0; warnings=60. |
| Presented water motion | `candidate-local-water-motion-scene07` | PASS local-coordinate reference-fluid model at dt=0: all 7,662 visible final-frame water pixels move. Maximum error vs projection of an 8 cm container translation is 3.07229e-5 NDC, inside the predeclared FP32-world/FP16-storage bound. Initial reset, resize and shutdown pass. World-coordinate solver numerics are covered separately. |
| Environment/clouds | EnvironmentGpuTests in completed smoke runs | PASS exact original-pass baseline and serial/parallel images for off/DDS/atmosphere/clouds/advanced phase and lifecycle cases. |
| AO / SSR | `ao-ssr-coverage.csv` | PASS 20 cases / 40 pairs, indexed to matrix02–06 and ssr-matrix07: SSAO/HBAO/XeGTAO/CACAO × full/half × early/late, and four SSR qualities. Each PASS row follows image comparison and InfoQueue validation. Not rerun after scope narrowed. |
| Native post / UI, historical | `candidate-post-matrix04`, cases 0–3 | PASS 3 frames each: disabled DoF/blur exact HDR pass-through, visible enabled effects, native color identity, nonempty posters/effects, 682 opaque green UI pixels verified in final display pixels. Tests of other providers were subsequently removed. |
| FSR Debug | `candidate-fsr-correctness01` | PASS 40 paired frames with real 1/60 simulation, default stability, NativeAA/Quality/Balanced/Performance/UltraPerformance, camera cut, context restart and resize to 897×608. Requested/effective FSR=2, no fallback, output extents and finite HDR verified. Errors=0; warnings=131. |
| FSR Release | `candidate-fsr-release-correctness02` | PASS same FSR-only protocol in Release. This is correctness validation with the debug layer, not ordinary timing. |
| Final transient-tail policy | `candidate-transient-tail-build48`, `candidate-transient-tail-release-build04`, `candidate-fsr-transient-tail03`, `candidate-fsr-final-release04` | PASS final Meta Debug/Release builds and 40 paired FSR frames in each configuration. Final Release: 88.16 s, errors/corruption/discarded=0, warnings=131. No performance interpretation. |
| Legacy regression evidence | `legacy-base-default01`, `legacy-base-custom01` | Original BaseRenderPipelineTests default/custom GPU paths passed. Final production App Debug/Release builds passed (`legacy-final-debug-build02`, `legacy-final-release-build02`), including the shared water signature and descriptor header. |

Engine.log appends. Read the named run's sliced `runtime.log`; older artifact folders
can contain stale files copied from earlier runs. A frame marker, per-run source
snapshot and current buffer manifest are required. The runner now copies only files
written during that runtime. Full InfoQueue export includes storage filter details:
the observed filter denied INFO (severity 3) only, with no denied error severities,
IDs or categories. Discarded-message overflow remains fatal.

## Fixes

Commit `791db96c1a79c7f4eb2818dda6dd0c20c7285b83` repaired the missing shadow include,
obsolete startup invariant, CPU test source lists, retained backbuffer owners during
resize/recreation and invalid SRVs bound while recording probe shadow depth.
Temporary termination/PDB diagnostics were removed after the CRT abort was traced to
swapchain resize. Debug packet names and complete InfoQueue reporting remain.

Subsequent corrections:

- Water optics and transparent meshes now receive the current path's frozen probe
  radiance SRV. The shared global descriptor could refer to the other paired path's
  published texture on the next frame. Legacy water callers keep the default SRV.
- XeGTAO depth prefilter declares its bound normals SRV read. SSR preparation omits
  radiance/fallback SRVs while the fallback resource is being written as UAV.
- Internal deferred and far/near WBOIT targets use the reconstruction render grid.
  They previously remained at window resolution when FSR/XeSS changed input size.
- Slot-local AO and paired owners exceeded 399 transient descriptors. The final
  Meta-only policy allocates new generic transient views in heap tail 105000+;
  the legacy 399-slot range and fixed shader-array domains are not widened.
  Exhaustion errors identify heap type/start/count. The validation HDR owner is
  destroyed after executor drain on exception unwinding.
- Paired scene validation rasterizes posters once, then both paths read the same
  completed texture. This preserves the original backend ownership ring capacity.

## Image oracle

The original exact final-HDR test exposed both the probe-SRV defect and legal atomic
particle append permutations. A controlled diagnostic copied reference draw records
into the candidate: all eight scene images then matched exactly after the SRV fix.
This was causal evidence only. The substitution and its environment switch were removed.
A DIRECT/DIRECT reproduction also exhibited the original additive discrepancy.

The checker requires exact particle state/counters/dead lists, identical alive-index
membership, identical canonical draw records, ordered weighted/sorted records and
identical additive membership. Far HDR, water optics, near WBOIT accumulation /
revealage, water depth/motion and opaque depth remain exact. It does not change
sorting, rendering quality or the solver to make a comparison pass.

Only accumulation-order differences use an arithmetic bound. For n contributions,
u is one full ULP (2^-10 for FP16, 2^-23 for FP32), gamma=n*u/(1-n*u). Nonnegative
sums use 2*gamma/(1-gamma)*max(a,b) plus 2*n*minimum-normal for underflow. Signed
particle XY sums use the absolute-sum bound 8*gamma*n; the static-camera billboard
fixture checks both projected positions inside [-2,2] before applying it. Nonfinite
values and n*u>=0.5 are rejected. Per-frame CSVs retain changed-value counts,
maximum absolute error and fraction of the bound. These conservative arithmetic
bounds are not image-quality tolerances fitted to a failed screenshot. See
[Microsoft floating-point rules](https://learn.microsoft.com/en-us/windows/win32/direct3d11/floating-point-rules)
and [output-merger precision](https://learn.microsoft.com/en-us/windows/win32/direct3d11/d3d10-graphics-programming-guide-output-merger-stage).

FSR additionally requires its requested provider without fallback, exact unchanged
HDR inputs through disabled post effects, correct display-sized output, all-finite
motion/color samples and HDR peaks above 1. At Debug default frame 15, input peak
RGB=27.2188 and FSR output peak=29.4375 (not clipped to LDR). Tiny negative ringing
values are preserved, not clamped by the test. `fsr-checks.csv` records preset,
render/output dimensions, history/context generations, simulation dt and allocation
statistics; `geometry-N-fsr-numeric.csv` records image metrics. All four completed
FSR protocols retain `fsr-workload.csv`: each frame has 80 actual rendered particles
(64 additive, 8 weighted, 8 sorted alpha) and dt=1/60; no empty-workload pass. A decoded display
PNG was inspected and contains the expected geometry, grass, water and particles.

## Historical measurement plan (withdrawn by user)

This is a synthetic supported-technique fixture, not the full production Bistro
scene. No Bistro FPS, ordinary renderer A/B speedup or GPU-overlap claim is made.
Future timings require frozen matching scene/settings/seed/clocks/counts, three
alternating fresh-process pairs (120 warmup + 300 scored), a 120+1800 candidate
run, raw samples and scatter. CPU recording mode and queue placement are separate
axes. Disconnected-session timings must prove that the workload and real simulation
steps remain active and must be labelled accordingly. Actual GPU overlap requires
a separate timeline. Independent review of the final source and these evidence
limits is still required before U1–U8 acceptance or starting U9/U10.

The compile database was regenerated with `tools/Generate-CompileCommands.py`: 44
projects / 163 translation units, including the three new GPU test sources. The
smoke runner passed PowerShell syntax parsing; its full smoke suite was not rerun.
Original checkout preservation was verified again after the final workloads.

The user selected Git commits on `codex/meta-render-graph-u1-u8-validation` for
transfer and review. The prepared bundle/archive remain local and are not the
transfer mechanism. Source fixes are committed as `791db96c` and `a287c9a5`.
`validation-evidence/` carries compact raw numeric tables, run statuses and a
manifest with source/binary fingerprints and selected raw path/hash references.
Large images, buffers and traces remain on MisSpec; the full local index records
64,760 files / 50,525,720,744 bytes. No ordinary FPS or GPU-overlap result is claimed.
The branch remains an intermediate correctness checkpoint pending independent
review and the measurements listed above.

## Follow-up benchmark freeze and missing interaction coverage

The focused `--particle-interaction` test passed in Release with the debug layer:
known field (.25,.1,.75,30), nonzero dt=.1, independent CPU displacement/velocity
oracle, disabled-field negative control and DIRECT/COMPUTE parity. Maximum error
9.53674e-7; debug-layer errors/corruption/discarded=0, warnings=2. The field is
produced by a DIRECT copy and declared read by the simulation packet; the exported
schedule retains its cross-queue dependency. Earlier suites were not repeated.
`independent-raw-release.json` independently decodes saved FSR frames15/39 with
Python struct/hashlib; it does not use the C++ checker or numeric CSV as an oracle.

The reproducible benchmark runs the same synthetic geometry/water/particle scene,
with a benchmark-only lifetime of120s and bounded particle volume70cm to retain
its80-particle load through the long run. Water movement is bounded20*sin(frame/60).
Native and FSR proof replays each retained80 particles and meaningful visible water
at warmup/end and the short/long midpoints; their checkpoint readbacks exclude them
from ordinary timings. Actual water work is5 substeps/frame (9000 scored steps in
each long replay). Jolt is not registered in this host and is not claimed tested.
`benchmark-protocol.json` freezes the executable/DLL/shader/config hashes, exact
fresh-process order and120+300 /120+1800 counts before ordinary measurements.
Performance results and a separate GPU timeline remain pending at this checkpoint.

## User stop and revised scope

The user explicitly stopped all further tests, builds, benchmark comparisons,
long runs, timelines/profiling and additional matrices. Scope is now the already
obtained minimal GPU-result correctness; the earlier performance requirements are
withdrawn. The owned benchmark runner/process tree was terminated, logs preserved,
and unfinished runs marked cancelled rather than PASS. See
`validation-evidence/benchmark-cancellation.json` for the completed-prefix and
cancelled/unstarted entries. There is no complete A/B series or performance claim.
The benchmark instrumentation/protocol commit remains historical review material;
it is not an accepted feature. No more workloads are authorized by this checkpoint.
U9/U10 remain blocked pending independent review and a separate user instruction.

The user's final acceptance criterion is now only: starts, submits GPU work,
produces a result and has no obvious crash/error. Earlier exactness/tolerance,
edge/variant/parity matrices and performance gates are withdrawn. No additional
run was made after the stop. Plan/AGENTS policy updates are owned by the reviewer.
Unfinished benchmark runtime code is isolated at local branch
`codex/meta-render-graph-benchmark-wip` / `df572e23be8a16f2fc9c1bd386e79d5661ddfaed`;
it is removed from the current task-branch runtime without rewriting history.
The completed focused interaction fixture and saved raw evidence remain available
for source review, without becoming mandatory acceptance gates. Historical numeric
tables/protocols remain evidence only and do not require repeating their checks.

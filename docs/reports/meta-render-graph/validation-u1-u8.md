# U1–U8 validation on MisSpec

Date: 2026-11-14 (Europe/Moscow). **In progress; phase not accepted.** U9/U10
remain blocked pending independent acceptance. No U11, Bistro merge, PR or push.

Frozen base: `ee20d6f8dd47e9cc92402a03397125c9cc6643d2`, tree
`03ee597493fb4cdf4c92a994e0cfab9382c61bce`.
Task branch: `codex/meta-render-graph-u1-u8-validation`.
Worktree: `C:/Users/MidSpec/.codex/worktrees/meta-render-graph-u1-u8-validation/PEPEngine2`.
Local evidence root (paths below are relative to this worktree):
`Build/Verification/meta-render-graph-u1-u8/`.

## Ownership and preservation

The previous U6 chat was idle and no build/engine/test workload was running at
claim time. The atomic exclusive claim is in the original checkout at
`.tmp/meta-render-graph-validation/owner.json`, owner chat
`01a1d930-ecb6-7251-99a2-4c4785c3c5a5`, scope U1–U8. Every workload rereads that
claim and checks competing processes. Workloads are serialized, including CPU
tests and builds. The historical D-drive MGPU lease remains released and untouched.

Original U6 HEAD is `2ee25daede82062a27bcd9d2add2d0b3ffbdb71c`. Its five staged
fixes and foreign untracked `Apps/App/App.vcxproj.user` are preserved. Original
staged patch SHA256: `00925DF035CC4FA0808BC972678CA838BE730E7C13F278C8DB0BD9AD23D931AA`.
Unstaged patch was empty. Foreign user file SHA256:
`C06A75B13F855A94D46616796E024C52B499F8F92CF00CCB571DDBC6FF574676`.
No reset/clean/stash was used. Worktree submodules and 614 cached LFS payloads
were hydrated separately; old executables are not evidence.

## Coverage and declared oracles

Runtime evidence currently uses Debug x64, VS18 Professional/v145, one
RTX 5050 device. Builds use `/m:1 /nr:false`; output/obj are worktree-local and
separate by configuration/project. Build/run JSON, complete stdout/stderr and
MSBuild logs remain in the evidence root. Labels below identify those files.
`pass` applies only to the named oracle, never the whole unit.

| Requirement | Test / command or label | Oracle | Current result / gap |
|---|---|---|---|
| U1 fresh experimental build | `frozen-meta-debug-build`, `candidate-meta-debug-build02` | Fresh compile/link with own output/config | Frozen source failed missing texture include; candidate Debug build passed. First relink failed on unhydrated Wwise LFS pointer; hydration resolved it. Candidate Release build passed (`candidate-meta-release-build01`); legacy builds not-run. |
| U1 startup single-device/local-only | `MetaApplication::PrepareStartup`, smoke | Primary=0, both secondary adapters=-1; local-only adapters; one initialized device, no offload | Obsolete absent-water-CVar invariant replaced with explicit water adapter=-1. Full poison/config/hash oracle pending. |
| U2/U8 CPU dependencies/states | `candidate-plan-debug-run01` / PlanTests | RAW/WAR/WAW, all readers, UAV, cycles, decay; COPY reuse joins DIRECT+COMPUTE readers; invalid states rejected; fixed heaps never transition; imported texture release | **pass**. Target source-list link defect fixed. |
| U3 nested recording | `candidate-recording-debug-run01` / RecordingJobsTests | 0/1/37 indexed buckets, delays, nested continuation, one/four threads, serial/parallel, error/cancel/shutdown | **pass**. |
| U2/U3 DIRECT GPU execution | DirectExecutionGpuTests in smoke | Exact `4*x+3` reader values, `8*x+7` result, exact pixel, disjoint buckets, prefix/suffix sentinel; early submission; failed tokens; delayed reuse | Fixture reports pass. In first run delayed slot wait=152 ms. Overall scene debug layer still fails; do not infer clean fixture layer from final whole-run failure. |
| U8 real queue chain/fork/join | Added async invocation in DirectExecutionGpuTests | Same exact values/pixel on actual COPY/COMPUTE/DIRECT native list types; two slots; serial/parallel; every token issued | Fixture reports pass for eight frames. Schedule exported as `async-schedule.txt`. Late wait/prefix GPU-progress and async error/cancel fixtures remain pending. No overlap claim. |
| U4 geometry/skin/shadows/grass | Scene smoke geometry readbacks | Exact canonicalized serial-DIRECT vs parallel-async scene buffers/images; skin expected pose tolerance 0.001; eight frames and three slots | All eight comparisons and queued-slot readbacks completed in `candidate-meta-debug-smoke03`; whole run **fail** on D3D12 error. Not a Bistro acceptance. |
| U5 environment/clouds | Existing original-pass environment and lifecycle fixture in smoke | Existing exact original-pass/serial/parallel readbacks for off/DDS/atmosphere/cloud cases; lifecycle histories | Runs as part of smoke; final metrics/index pending. Full AO early/late, all implementations and probe/cache/SSR histories pending. |
| U6 water | New executor with actual FluidInstance | Same seed/inputs/real dt/steps; numeric state and combined depth/motion/image tolerance declared before run | **not-run**. Current host contains no water component; linked water code is not water execution evidence. |
| U6 particles and transparency | New executor mixed-group fixtures | Birth/death/burst/reset/pause/timeScale, collisions, equal real steps, global interleaved SortedAlpha and shared WBOIT | **not-run** beyond inherited single additive burst. Full state/numeric/image oracle required. |
| U7 native post/UI/history | Scene smoke | Color/depth/motion, UI/posters, camera cut/reset/resize readbacks at declared tolerance | Native path executes, but no post image/history oracle yet. D3D12 error blocks acceptance. |
| U7 providers / FG | Per-provider focused runs | Effective selected/fallback status plus input/output/history and presentation lifecycle | **not-run**. No provider marked unavailable without actual capability evidence. |
| U8 resize/shutdown | Scene smoke with retained screen composition owners | Drain before releasing owners and before both explicit resize and provider-triggered recreation; no stale backbuffer references | Before fix CRT abort in CreateSwapChainForHwnd. After fix resize and normal destruction complete; overall layer failure remains. Provider/suspend recreation pending. |
| Debug layer / GBV | `diagnostic-meta-debug-smoke04` | All messages saved with IDs, severity and discarded counts; zero ERROR/CORRUPTION and zero overflow | `candidate-meta-debug-smoke05` **pass**: ERROR/CORRUPTION=0, warnings=87, discarded=0. Prior named run had 630 errors (IDs 538/527). Focused GBV not-run. |
| Legacy regression | Fresh App Debug/Release + affected legacy suites | Existing production oracles; configs unchanged | **not-run**. |
| Ordinary performance | Fresh Release A/B, below | Frozen equivalent inputs, raw samples and all runs retained | **not-run**; gated on correctness. |
| CPU/GPU timelines | Separate instrumented runs | Real recording spans and queue work/waits/overlap | **not-run** for this candidate; queue existence proves no overlap. |

## Performance protocol (before collecting timing data)

For each key comparison use at least three alternating fresh-process pairs,
120 warmup frames followed by 300 scored frames per process. Freeze executable
SHA256, source commit/diff, configuration, scene/camera, seed 12345, all counts,
quality, real applied controls, GPU/driver, workers and workload. Retain slow,
failed and timed-out runs. Changed fingerprints are failed/censored evidence.
Use ordinary Release without debug layer, GBV, profiler, FG, VSync or frame cap.
Report scatter, median, mean, p95/p99, CPU recording/submission and owner sections,
GPU time, memory, simulated seconds/fixed ticks/debt and actual water steps/counts.
Follow with a longer candidate run: 120 warmup + 1800 scored frames.

Meta single-DIRECT vs Meta async uses `Meta.Async.Queues=false/true` on the same
frozen scene. CPU serial/parallel recording is a separate axis. Legacy-vs-Meta
requires the same supported scene and settings. The current Meta host always loads
SmokeWorld and adds synthetic U4 geometry/grass/emitter, even outside `--smoke`;
its FPS cannot be compared to production Bistro as an executor speedup. The
matching legacy/new host path has not been established yet. No scene/quality or
physics reduction may be used to improve candidate numbers.

## Fixes and current failure

Minimal fixes so far: missing SceneShadows texture-resource include; explicit
disabled water secondary adapter at startup; CPU test targets scoped to actual
CPU sources; presentation owners released after executor drain before both
window resize and FG/provider recreation. ScreenComposite recordings retain a
COM handle to the target; freeing them only in pipeline Resize occurred after
swapchain recreation and caused the observed abort.

`diagnostic-smoke02-artifacts/Engine.log` contains the PDB stack through
GDX12DeviceFactory::CreateSwapChain → GDX12SwapChain::Resize → RenderModule::OnResize.
`candidate-meta-debug-smoke03-artifacts/` preserves all subsequent raw outputs;
its final error is ClearDepthStencilView with assumed SRV state instead of
DEPTH_WRITE. Temporary FatalDiagnostic/PDB instrumentation was removed after
diagnosis; Release build validates the cleaned host. Debug packet labels and complete InfoQueue export make the next
failure attributable and remain fatal, including discarded-message overflow.

The reflection shadow recorder incorrectly bound lighting SRV slots 3–9 while
writing the sun depth target. Matching the production shadow binding subset
removed all 630 validation errors; full scene smoke05 passed. The copied artifacts
include every InfoQueue message and severity/discard counts.

No performance, full U6/U7 correctness, complete lifecycle or phase
acceptance claim is made. This report will be updated with exact candidate commit,
binary/config hashes, numeric tables and artifact manifest as validation proceeds.

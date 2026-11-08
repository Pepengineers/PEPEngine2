# Debug Bistro: CPU overhead reduction and the 20 FPS limit

Local commits: `798ce8b` (benchmark samples/physics telemetry and grass marker),
`d6eb1b4` (optimizations and regression tests). No push.

8 November 2026; Ryzen 7 5800HS / RTX 3060 Laptop 6 GiB, Windows 11.
The final three alternating A/B pairs improved the median run mean from
**64.3901 to 49.4348 ms (15.53 to 20.23 FPS, +30.25% FPS)**.
All three candidate means were below 50 ms. **Sustained 20 FPS was not achieved:**
the subsequent 1200-frame run averaged **55.0362 ms / 18.17 FPS** and accumulated
6.2395 seconds of additional physics debt. The short series is not a stability claim.

## Frozen workload and measurement

Baseline application sources: `95ae243`. The parallel scene task subsequently
committed `17ac9a7`; its route changes were preserved. Both sides of the scored
comparison compile App.cpp against the same frozen 95ae243 scene headers, with
the identical benchmark telemetry change in App.cpp. The frozen tree contains
all App sources, but only App.cpp is recompiled by build_frozen.py; other renderer
TUs and engine libraries vary with the accepted optimizations below. The current-scene normal App result is reported separately.
The source hashes are in [frozen-inputs.json](2026-11-08-debug-bistro-optimization/frozen-inputs.json).

Each process uses 120 warm-up + 300 measured frames, in order A1/B1/A2/B2/A3/B3.
The ordinary Debug executable has no diagnostic CPU timers and no Nsight attached.
The existing benchmark waits for uploads once after the measured batch; there is
no per-frame CPU-drain wait. `frame_ms` includes this final drain; raw per-frame
samples exclude it. FPS = 1000 / run mean; the reported median is over three run means.
The raw samples are retained without outlier exclusions.

Camera (-300,200,20), rotation (0,117,0), 1280×800, frames-in-flight=3,
Settings visible, bounds=false, physics×1, particles×2 / 500000 budget,
NPC=10, fox=5, grass=60000. Character benchmark seed=12345; wave seed remains
12345. Interactive startup retains random character seeds. Geometry, counts,
materials, shadows, probes, AO, SSR, water, grass, particles, animation cadence,
60 Hz physics, CCD, solver/collision-step settings and wave impulses are retained.
Realtime windows with the same seed still visit different simulation phases.

All runs unset PERF_JOLT/PERF_CONTROLS/PERF_SOAK. Runtime INI was byte-restored,
SHA256 `53c782371018e3e7e00f43d7d41594de0112be828a89277c076651a86b16e245`.
The harness checks executable/INI fingerprints, shader hashes, frame counts,
scene counters and requested − simulated = debt delta (within JSON rounding).
Baseline uses its original two-constant LocalShadowPass shader; the candidate
uses its matching three-constant shader. The other shader inputs are unchanged.

The compiler retains `/Od /RTC1 /MDd /ZI`, `_DEBUG`, assertions and iterator checks;
there is no `/O2` or Release measurement. Evaluated command lines are retained in
[debug-flags.json](2026-11-08-debug-bistro-optimization/debug-flags.json).
**The final App Debug link explicitly sets `LinkIncremental=false`**. This removes
incremental-link thunks while retaining full PDBs and Debug runtime checks, but
loses incremental linking / Edit and Continue for App. This configuration change
is part of the delivered result and is not attributed solely to C++ caching.
Baseline remains `/INCREMENTAL`.

## Three alternating pairs

| Run | Mean ms | FPS | Raw p50 ms | Raw p95 ms | Raw min–max ms |
|---|---:|---:|---:|---:|---:|
| [A1](2026-11-08-debug-bistro-optimization/accepted-A1.json) | 62.5840 | 15.9785 | 61.7343 | 68.9271 | 57.5069–73.9142 |
| [B1](2026-11-08-debug-bistro-optimization/accepted-B1.json) | 49.0295 | 20.3959 | 48.9380 | 53.8660 | 42.9779–60.6770 |
| [A2](2026-11-08-debug-bistro-optimization/accepted-A2.json) | 64.3901 | 15.5303 | 63.3144 | 71.5312 | 57.8992–90.5099 |
| [B2](2026-11-08-debug-bistro-optimization/accepted-B2.json) | 49.4348 | 20.2287 | 49.0206 | 55.3795 | 44.1127–60.8755 |
| [A3](2026-11-08-debug-bistro-optimization/accepted-A3.json) | 66.0601 | 15.1377 | 64.9141 | 74.0541 | 57.9507–86.1263 |
| [B3](2026-11-08-debug-bistro-optimization/accepted-B3.json) | 49.9908 | 20.0037 | 49.1805 | 56.1514 | 44.6698–63.8525 |

[Raw frame scatter](2026-11-08-debug-bistro-optimization/frame-scatter.svg) shows
all 1800 samples. Candidate p95 exceeds 50 ms in every run: these are average
20 FPS results, not a 50 ms worst-frame guarantee. B3 cleared the mean threshold
by just 0.0092 ms, so the short-window margin is effectively exhausted.

NVIDIA telemetry is captured before and after each run, not continuously.
The series warms from 44 to 76 °C; B3 starts at 75 °C / 2055 MHz graphics and ends
at 76 °C / 2047 MHz; memory is 7151 MHz under load. Clocks were not locked.
CPU clocks/temperature were not recorded. Baseline drift across pairs remains;
small differences between trials cannot establish isolated causal gains.

## Physics time is reported separately

| Run | Requested s | Ticks | Simulated s | Debt start → end s |
|---|---:|---:|---:|---:|
| A1 | 18.7184 | 1154 | 19.2333 | 2.252450 → 1.737540 |
| B1 | 14.6708 | 926 | 15.4333 | 0.778166 → 0.015665 |
| A2 | 19.2659 | 1126 | 18.7667 | 2.832090 → 3.331330 |
| B2 | 14.7892 | 952 | 15.8667 | 1.083300 → 0.005807 |
| A3 | 19.7613 | 1121 | 18.6833 | 2.859880 → 3.937860 |
| B3 | 14.9559 | 961 | 16.0167 | 1.062190 → 0.001443 |

Times are seconds; ticks use the unchanged 1/60 s step. Candidate simulated time
can exceed requested time while paying down warm-up debt. Debt is reconstructed
from all requested time and executed ticks since benchmark startup, including
warm-up. No debt is forgiven, no ticks are skipped and no TimeScale is changed.

| Ordinary run | Frames | Mean ms / FPS | Requested / simulated s | Ticks | Debt start → end s |
|---|---:|---:|---:|---:|---:|
| [complete-soak](2026-11-08-debug-bistro-optimization/complete-soak.json) | 1200 | 55.0362 / 18.1699 | 65.9895 / 59.7500 | 3585 | 0.013435 → 6.252900 |
| [current-scene](2026-11-08-debug-bistro-optimization/current-scene.json) | 300 | 53.1049 / 18.8307 | 15.8908 / 14.3667 | 862 | 0.003250 → 1.527340 |

Current-scene App uses the unmodified 17ac9a7 routes with all final engine changes. It is a separate single trial, not an additional frozen A/B pair. The soak warmed the GPU from 75 to 83 °C (2047→2025 MHz); current-scene ran next, 83→81 °C (2025→2040 MHz). These are endpoint samples; CPU thermal contribution is unknown.

The 1200-frame ordinary candidate requests 65.9895 s and simulates 59.7500 s
(3585 ticks): 90.54% of the requested interval. Its debt rises from 0.013435 to
6.252900 s. Mean FPS is 18.17; p95 frame time is 67.4986 ms. This is approximately
one minute of measured work, not a multi-minute containment/stability soak.
Consequently the implementation does not satisfy a sustained 20 FPS / realtime
physics target. A previous, intermediate indexed-UI candidate also missed the
long-run target (52.0974 ms, debt +0.6153 s); it is not substituted for the final result.

## Accepted changes

- Scene snapshots retain unchanged registry records and cache geometry/material/
  transform signatures independently, including dynamic meshes. Consecutive
  submeshes share a transform hash. Static aggregate bounds/hash/casters rebuild
  on membership or metadata changes, including removal of an extreme. External
  primitives retain full scanning; actual instance/world mirrors are compared,
  so direct mirror edits and classification changes invalidate cached metadata.
- Settings cache the sorted registry snapshot, UTF-8 labels, group tree and filter
  result by registry revision. Each frame resolves indices into a fresh snapshot;
  values, validation, pending readonly values and search stay live. The cache owns
  no CVar pointers. Pose jobs compute the same pose-byte signature when it changes,
  moving hashing off the recording thread without reducing animation updates.
- Local shadows retain full templates for the static/legacy path and build a
  compact dynamic command stream from current instance classification. Logical
  count limits prevent high-water buffer tails from drawing. Proven empty dynamic
  faces still copy static depth. Counter reset uses an ordered zero-buffer copy.
  Common bindings are built once; reuse is guarded by tracked root signatures and
  disabled for unsplit application geometry sources. Skeletal batching/native
  invalidation restore full bindings. No rigid batching experiment is retained.
- Command-list helpers avoid temporary heap vectors when heaps match and forward
  all-needed barriers directly. Mixed/no-op transitions keep the filtering path.
- Dynamic-body synchronization still fetches every Jolt pose every tick and writes
  ECS edits back when required; it memoizes only the pure quaternion-to-Euler
  conversion when the quaternion is exactly unchanged. Body recreation and manual
  ECS edits are covered by tests. The physics backend/step budget is unchanged.
- App Debug uses explicit nonincremental linking as disclosed above. A grass-expand
  GPU marker permits per-shadow-view attribution without changing grass work.

## Attribution and rejected experiments

| Scope, ms/frame | Baseline, 300 | Intermediate compact, 300 | Final first 300 | Final all 1200 |
|---|---:|---:|---:|---:|
| Physics total | 20.138 | 14.755 | 16.466 | 18.798 |
| Jolt (inside physics) | 15.236 | 10.592 | 12.329 | 14.251 |
| Dynamic-body sync (inside physics) | — | — | 3.762 | 4.149 |
| Scene snapshot | 13.778 | 7.040 | 7.120 | 7.957 |
| Snapshot extraction (inside snapshot) | 6.020 | 4.566 | 4.720 | 5.314 |
| Snapshot aggregation (inside snapshot) | 7.513 | 2.306 | 2.225 | 2.455 |
| Settings/UI begin | 5.815 | 2.603 | 1.557 | 1.591 |
| Skeletal preparation | 4.989 | 3.738 | 3.903 | 4.445 |
| Local-shadow recording | 10.967 | 7.409 | 7.434 | 8.876 |
| Grass expansion CPU (inside geometry) | 1.017 | 0.704 | 0.723 | 0.859 |
| Grass view preparation | 0.876 | 0.647 | 0.678 | 0.807 |
| Present | 0.311 | 3.605 | 3.034 | 1.239 |
| GPU timestamp elapsed | 46.020 | 47.264 | 47.899 | 47.002 |

These are separate instrumented runs, not ordinary FPS evidence. Parent and child
scopes overlap and must not be summed. Active-body counts and simulation phases
vary; timing differences, especially Jolt, are not proof that its solver got faster.
The production change does not alter solver work. The final profile uses 120 warm-up + 1200 measured frames on frozen 95ae243 scene headers. Its ordinary-style frame mean (55.5424 ms) is diagnostic only. The final first-300 column avoids silently comparing a 1200-frame mean to a 300-frame mean. The intermediate compact profile predates indexed Settings, quaternion memoization and guarded root reuse.


Final fixed-step attribution: Jolt 14.251 ms + dynamic-body synchronization
4.149 ms + pre-physics kinematics 0.152 ms + wave callback 0.055 ms per frame;
these are already inside physics=18.798 ms. Total bodies=950, average active=603.49.
The wall-time step limit triggers in 85.83% of measured frames. In the last two
300-frame windows it triggers on every frame; active bodies average 700.78 / 616.17,
physics costs 20.449 / 20.474 ms and debt ends at 3.089 / 4.105 s. GPU timestamp
elapsed remains 46.35 / 46.53 ms in those windows. This narrows the remaining CPU
problem to actual Jolt and post-step synchronization under the moving/contact
workload; it does not establish a final GPU-only bottleneck. Reducing Settings
alone cannot recover the remaining long-run budget. Diagnostic timing and body
state differ from the ordinary soak, so its 4.105 s debt must not replace the
ordinary soak's 6.253 s debt. Per-window details are retained in
[profile-shipping-windows.json](2026-11-08-debug-bistro-optimization/profile-shipping-windows.json).

A validated four-frame Nsight 2026.4 trace of an intermediate candidate measured
mean frame interval 53.1171 ms, GR active 98.13–98.18%, SPH 17.5420 ms,
Local shadows 13.5562 ms and Sun shadows 7.2347 ms. Local draw=6.9068 ms and
cull=1.8436 ms are children of Local shadows. Grass expansion totals 3.4305 ms
across 31 views: 2.4603 ms inside Local and 0.9702 ms inside Sun. These nested
costs are already in the parent passes. No exported marker rows were excluded.
The trace predates compact commands and final CPU changes; it demonstrates the
remaining GPU cost of unchanged water/shadows, not final GPU speedup or ordinary
FPS. [Validated GPU breakdown](2026-11-08-debug-bistro-optimization/gpu-validated.json).
Capture: `Build/Debug20Optimize-20261108/2026-11-08-03-58-07-448/Debug20OptFinal_2026_11_08_03_59_01.ngfx-gputrace`.
The wrapper returned 4 because it expected BASE instead of 2026.4 BASE_UNLOCKED;
ngfx export itself returned 0. A local argv-format shim corrected the wrapper's
single-argument quoting before this successful capture; the failed pre-DX12
attempt is excluded from evidence.

Accepted/rejected selection is explicit:

| Experiment | Evidence / decision |
|---|---|
| Snapshot, Settings and pose-signature caching | Accepted; invalidation/lifetime tests retained |
| Compact dynamic shadows, empty-face and command/root reuse | Accepted; full-scan depth/HDR oracle passes |
| Unchanged-quaternion Euler cache | Accepted; no step/solver/cadence change |
| App `/INCREMENTAL:NO` | Accepted, explicit project setting; full Debug checks retained |
| Rigid local-shadow batching prototype | Rejected and removed: 56.5617 ms / 17.68 FPS despite passing image tests |
| Water+particles async | Rejected as a default change: first trial 57.6311 vs 52.9194 ms; later 49.8863 vs 49.1635 ms; not a repeated win |
| N→N+1, quality/count/physics reductions | Not implemented |

An early frozen-link helper accidentally appended `/INCREMENTAL:NO` behind the
project's `/INCREMENTAL`. Early single trials (FirstThree through CompactBulk)
are exploratory and cannot be called a matched-link A/B comparison. The helper
was corrected. A subsequent three-pair **matched `/INCREMENTAL` intermediate
code-only** series gave A=63.5529/66.2905/68.3300 ms and
B=51.5975/57.3795/56.0179 ms: medians 66.2905→56.0179 ms (+18.34% FPS),
still below 20 FPS. Later explicit-NOINC Deliver pairs gave
B=49.6583/51.1001/51.2025 ms; their median also missed 20 FPS.
Only the final accepted series at the top includes the last indexed-Settings,
Euler-cache and guarded-root-reuse changes together. A trial tagged
`hot-noincremental` actually started at 51 °C; it is a cold single run, not hot
validation. Original preliminary baseline=79.9245 ms also is not substituted
for the three-pair median baseline.

## Verification and reproduction

- Final ordinary Debug App build and final Release App build passed (MSBuild errors=0). Release was built for compatibility; no Release FPS is used. Existing third-party missing-PDB / compiler warnings remain.
- RenderPipelineTests, ObjectDataJobsTests and InstancingTests were rebuilt and passed on the final component/layout definitions (`shipping-*` states).
- LightingGpuTests and BistroRenderGraphTests passed after the final root-reuse change (`roots-*` states): cached/compact versus full-scan depth and HDR, point/spot cubemap faces, D3D12 debug layer, odd resize, Native/XeSS, both AO placements and async transitions.
- Instancing reports zero relative HDR drift, 12→6 draw commands, all 12 visible instances, and preserved individual human/fox animation phases. This remains the existing optional instancing behavior; it is not enabled to inflate the benchmark.
- PhysicsTests passed after quaternion memoization, including unchanged-quaternion/manual-ECS edits, recreated body rotation, existing lifecycle/CCD checks (`physics-cache-tests-final`).
- Fresh standalone ConsoleVariableTests and SettingsCacheTests builds/runs passed through the checked-in reproduction script (`settings-repro.log`). This run executes the newly built binaries, not an earlier executable from another output directory.
- `git diff --check` passed. All builds, GPU tests, benchmarks and captures were serialized.


The full CPU/GPU logs, executables/PDBs, diagnostic TUs and frozen App sources
remain under `Build/Debug20Optimize-20261108/` (ignored). Compact raw samples,
telemetry, state fingerprints, shader manifests, flags, source hashes and profile
summaries are in the [evidence directory](2026-11-08-debug-bistro-optimization/).

From the repository root, build the ordinary App with MSBuild:

```powershell
$msbuild = 'C:/Program Files/Microsoft Visual Studio/18/Professional/MSBuild/Current/Bin/MSBuild.exe'
& $msbuild Apps/App/App.vcxproj /p:Configuration=Debug /p:Platform=x64 /m:1 /nr:false
& $msbuild Apps/App/App.vcxproj /p:Configuration=Release /p:Platform=x64 /m:1 /nr:false
```

For a current-scene ordinary measurement, use the fixed INI above and run from
`Build/bin/x64/Debug/App`:

```powershell
.\App.exe '-Loading.BenchmarkWarmupFrames=120' '-Loading.BenchmarkFrames=301' '-Loading.BenchmarkOutput=D:/Project/PEPEngine2/Build/debug-current.json'
```

Use BenchmarkFrames=1201 for 1200 samples. Existing benchmark switches are
unchanged; character-seed/physics/raw-sample fields are emitted with the report.
Keep each `-Loading.*` argument quoted in PowerShell: unquoted arguments split
into `-Loading` and `.Benchmark...`, causing an ordinary interactive launch.
The exact frozen comparison harness is retained as
`Build/Debug20Optimize-20261108/build_frozen.py`, `measure.py` and
`run_accepted_pairs.py`; run it sequentially, never alongside builds/tests.
It compiles the frozen App.cpp TU with evaluated ordinary Debug flags and links the
respective libraries; it restores matching shader/INI bytes after each process.
Reconstructing baseline needs the 95ae243 libraries plus the App.cpp-only
telemetry patch from 798ce8b; building only current HEAD cannot reproduce the frozen baseline.

CPU registry/UI tests have a durable standalone build command:

```powershell
cmd /c docs\reports\2026-11-08-debug-bistro-optimization\test-settings.cmd
```

[test-settings.cmd](2026-11-08-debug-bistro-optimization/test-settings.cmd) compiles
and runs both ConsoleVariableTests and the new SettingsCacheTests with `/Od /RTC1
/MDd /D_DEBUG`; it does not depend on an ignored prebuilt test executable.
The new UI test is standalone, not added to the solution. GPU test projects can
be built using the same Debug command with their `Apps/App/*Tests.vcxproj` path.
The run_job states retain binary fingerprints and exit status; the harness scripts specify cwd and timeout.

Remaining limits: no final Nsight capture or CPU clock telemetry, no multi-minute
containment soak or second GPU, and no manual Settings interaction session.
These results apply to the measured laptop/workload. Sustained 20 FPS remains
open; the next optimization needs to reduce actual physics/GPU work without
lowering the requested fidelity, rather than hiding accumulated physics time.

## Independent review of the completed Sol High task

The parent chat reviewed commits `798ce8b`, `d6eb1b4` and `60c2a90`, including
snapshot invalidation, CVar owner lifetime, pose signatures, compact shadow
uploads/root reuse and quaternion memoization. No blocking runtime defect was
identified. One reproducibility defect was fixed above: PowerShell splits
unquoted `-Loading.*` arguments; an argv probe reproduced it.

All six accepted raw sample sets were checked against their run states and
physics accounting. Recomputed medians are 64.3901 and 49.4348 ms (+30.253% FPS).
The BistroRenderPipeline source-hash difference in the current checkout is
only CRLF versus LF; normalized content matches the recorded frozen source.

ConsoleVariableTests and SettingsCacheTests were freshly compiled and passed.
RenderPipelineTests, PhysicsTests and LightingGpuTests were independently
rerun and passed; their binary hashes match the worker's final tested binaries.
Lighting's depth/HDR checks passed with D3D12 debug layer enabled. The parent
did not repeat the complete Debug/Release build or the other regression suites.

Two further ordinary App runs used the actual current scene, identical executable
and INI hashes, 120 warm-up + 1200 measured frames, and no diagnostic timers:

| Control | Mean ms / FPS | Raw p95 ms | Raw frames >50 ms | Requested / simulated s | Debt start → end s | GPU start → end °C |
|---|---:|---:|---:|---:|---:|---:|
| [First](2026-11-08-debug-bistro-optimization/independent-current.json) | 49.8492 / 20.0605 | 55.3637 | 629/1200 | 59.7673 / 59.5333 | 0.014608 → 0.248525 | 44 → 73 |
| [Repeat](2026-11-08-debug-bistro-optimization/independent-hot.json) | 51.5958 / 19.3814 | 59.5212 | 819/1200 | 61.8642 / 61.6000 | 0.008862 → 0.273037 | 58 → 79 |

The second artifact's `hot` name denotes a sequential repeat, not a controlled
thermal plateau: it started at 58 °C, and CPU temperature/clocks were not captured.
Both process-boundary GPU samples begin at idle clocks. These controls support
the lack of margin at 20 FPS; they do not isolate the cause of the variation or
replace the frozen paired comparison. Neither provides a sustained 20 FPS claim.
The executable and INI fingerprints remained unchanged throughout both runs.

[Independent review metadata and telemetry](2026-11-08-debug-bistro-optimization/independent-review.json)
and the `independent-*` run states/test logs retain the checks. No second GPU,
attached debugger, multi-minute soak or manual Settings session was tested.

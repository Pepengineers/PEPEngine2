# Meta command-list preparation: ordinary Release A/B

2026-11-17. Baseline source HEAD `c5d5c132`; candidate changes only
`Apps/App.MetaRenderGraph/src/Graph/MetaGraphExecutor.cpp`.
Existing BistroPaintingSurfaces.h/BistroAdditionsTests.cpp WIP was preserved;
the painting change was present in both builds. No quality/configuration reduction.

## Change and decision

Keep the small CPU-work reduction:
- Omit prologue/epilogue command lists when their barrier arrays are empty.
- Reset each exclusively assigned allocator/list pair inside its recording task,
  after Begin has retired the frame slot. OwnerRecording callbacks remain on owner.
- Avoid the empty recording-bucket child for packets without worker payloads.
- Lists start marked closed, become open only after successful Reset, and are
  closed on callback failure; cancellation leaves untouched lists closed.

The existing per-slot/per-queue pool, capacity reservation, packet order,
ExecuteCommandLists/Signal boundaries, barriers and fence retirement are unchanged.
The terminal packet retains its payload list, even if empty, preserving submission
and completion semantics. This is not a new shared locking pool and does not
reduce pool high-water allocation. It removes work for lists that served no purpose.
Submission batching and compiler decay changes were deliberately not included.

## Method

Artifacts: `Build/recording-pool-ab-2026-11-17/`.
`measure.py`, `analyze.py`, `analysis.json`, six per-frame CSVs, six NVIDIA CSVs,
per-run engine logs, build logs, input hashes and measured source snapshots remain
there. Runtime binaries are `App.MetaRenderGraph.PoolBaseline.exe` and
`App.MetaRenderGraph.PoolCandidate.exe` beside the normal Release executable;
matching PDBs were saved with the artifacts.

Three sequential pairs, order A-B-A-B-A-B. Each fresh ordinary process ran
30 seconds of application frame time, without --smoke, PIX capture virtualizer,
debug layer or GPU capture. Same minimal temporary CPU telemetry was compiled into
both variants: per-frame dt/Update/Render durations, process kernel+user CPU time,
fixed-step callback count/simulated time, requested simulation time and derived debt.
The callback delegates to the original BeforePhysicsStep; physics math/timing
was not changed. CSV output is buffered. These are ordinary rendering-path runs
with lightweight CPU instrumentation, not completely uninstrumented executables.

Original analysis planned 10 seconds warmup + 20 measured. CSVs exposed first-frame
shader/resource startup stalls and physics catch-up after 10 seconds: the first
pair executed about 27 simulated seconds inside that 20-second window. That
window is unsuitable for steady-state attribution. Final analysis uses the same
**20–30 second interval for all six recorded runs**, after checking debt, without
rerunning or choosing windows by FPS. The initial summary is retained separately.
Each interval includes the frame crossing 20 seconds, so duration is slightly >10 s.

Frozen inputs: startup INI, both executables, runtime DLLs, shared/App shader
sources and named foreign WIP hashes, checked after every run (`inputs.json`).
Scene camera remains the default interior position (400,230,-370), rotation
(8,40,0). Ordinary config: three frame slots, XeSS default, physics Low with 256
dynamic glasses/wave participants; all other original prop categories static.
SPH target 100000, NPC=10, fox=5, atmosphere/daylight cycle, probes, DoF, grass,
particles and UI/cloud picture retained; cloud picture 1024×512 on primary.
Rendering uses existing default window settings; no camera/quality changes made.

## Results

CPU ms/frame is **process CPU time summed across threads**, divided by rendered
frames. It is not main-thread latency, a GPU timing, or the sum of nested scopes.

| Pair | A FPS | B FPS | FPS delta | A CPU ms/frame | B CPU ms/frame | CPU delta |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 22.33 | 21.52 | -3.64% | 42.27 | 40.94 | -3.14% |
| 2 | 25.10 | 24.62 | -1.90% | 42.10 | 35.17 | -16.46% |
| 3 | 21.29 | 23.49 | +10.38% | 42.77 | 40.92 | -4.33% |
| Mean of runs | 22.91 | 23.21 | +1.34% | 42.38 | 39.01 | -7.95% |

CPU reduction is observed in all pairs; median paired reduction is 4.33%.
This supports retaining the simpler recording path, but does not isolate reset
from empty-list removal or prove an exact repeatable 8% improvement.
FPS changes have mixed signs and substantial scatter: **no demonstrated FPS win**.
Frame-time means are 43.87 ms A versus 43.21 ms B. Per-run p95 frame times:
A 47.27/41.12/48.33 ms; B 47.82/41.77/43.71 ms.

| Run | Fixed ticks | Simulated seconds | Ending debt, ms | GPU utilization | GPU MHz | GPU °C |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 1-A | 602 | 10.033 | 1.11 | 97.8% | 1800 | 65.2 |
| 2-B | 603 | 10.050 | 3.28 | 98.1% | 1759 | 71.1 |
| 3-A | 603 | 10.050 | 1.94 | 98.4% | 1758 | 75.9 |
| 4-B | 602 | 10.033 | 4.89 | 98.7% | 1749 | 78.6 |
| 5-A | 601 | 10.017 | 1.22 | 98.4% | 1736 | 80.3 |
| 6-B | 602 | 10.033 | 15.10 | 98.0% | 1750 | 81.6 |

Debt stayed below one 60 Hz step in every final window; no accumulating physics
backlog or skipped simulation is credited as a rendering speedup. Physics is live,
not a bit-identical replay; rigid-body contacts and per-frame SPH work can vary.
NVIDIA samples are aligned to the final window using the ordinary shutdown log
timestamp, excluding process teardown. Mean GPU utilization: 98.20% A, 98.27% B.
GPU clocks/temperature drifted during the sequence; these short windows and three
pairs do not establish statistical significance. GPU pass timings were not captured.
The high ordinary GPU utilization is consistent with FPS being limited elsewhere
even when CPU work is reduced; a causal GPU bottleneck attribution was not measured.

## Verification and final state

All six ordinary processes exited successfully, generated completed CSVs, rendered
through the normal pipeline and had no ERROR/FATAL in their session logs. Frozen
hashes passed after each run. Source review checked disjoint allocator ownership,
fence retirement, barrier/payload ordering, OwnerRecording and cancellation cleanup.
No new tests, artificial failure runs, GPU-validation matrix, Debug A/B, PIX replay
or long soak were performed. The existing extended GPU fixture was not run.

Baseline sandbox linking hit LNK1101 (MSPDB140.DLL); host MSBuild `/m:1 /nr:false`
succeeded. Candidate and final host Release builds succeeded, with existing
C4244/LNK4099 warnings. Temporary MetaApplication instrumentation was restored
byte-for-byte and its source timestamp advanced for recompilation; the final
normal EXE was checked to contain no benchmark environment marker. No repeat smoke
after removing telemetry; the same executor change completed three measured runs.
Final EXE SHA256:
`0d62b935fd463614071d6fc949f66561215039ea668e0e91e7d4cc700e4c3164`.

Normal executable:
`Build/bin/x64/Release/App.MetaRenderGraph/App.MetaRenderGraph.exe`.
Task-owned changes are executor + this report only; no push. Further submission
batching requires a separate compiler/barrier change because buffer decay is
currently modeled at each packet submission.

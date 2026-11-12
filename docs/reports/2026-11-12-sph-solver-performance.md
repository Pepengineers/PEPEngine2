# SPH solver: particle dispatch order, 2026-11-12

Implemented two shader statements: after the Count guard, Densities and Integrate
replace dispatch index with `Sorted[i].y`. The first Count sorted records are a
permutation of live particle IDs. Each particle still visits the same cells and
neighbors in the same order and writes its original density/output slot. The
self-exclusion, coincident fallback and nonfinite restore all use that original ID.
Input/output particle buffers remain separate. No C++/resource/state/workload change.

Baseline is the accepted **B1024**, not the former global sorting network.
Production shader SHA256 before:
`fbbd54a025be866df6f07902842cb0afb51386a717cdc2cd3a35059f7f469649`;
after:
`54118a7a238396524de4864345d776c91e654a65e9adc06033453a4301235661`.
The frozen candidate shader used in the paired tests has exactly the latter hash.

## Full solver measurements

RTX 4080 SUPER / driver 610.88, normal Release /O2 fixture with runtime DXC cs_6_8
(-O3 shader compilation). DIRECT queue. Actual Bistro CreateFluidSource seed:
100980 particles, solver key capacity 131072, MGPUReference=true, near stiffness
2.15, H=.2, gravity -9.81, mass1, rest density630, viscosity0, mirrored wave speed6,
wave acceleration4/period6. Each frame executes all 3 substeps at Dt=1/180;
no quality, particle budget, interaction or simulated-time reduction.

Each repeat starts both variants from identical seeds, advances 120 warmup frames
(360 substeps, 2 simulated seconds), then measures 51 paired frames. Baseline and
candidate order alternates each frame. Exact complete GPU state is compared at
warmup end and after frame171 (513 substeps, 2.85 simulated seconds per variant).
Both variants generate actual hashes and interact with the evolving water;
keys are not random or substituted benchmark inputs.

| Repeat | B1024 median [p10;p90], ms | Candidate median [p10;p90], ms | Median reduction | Winning pairs |
|---|---|---|---|---|
| 1 | 5.117952 [4.501504;5.683200] | 1.529856 [1.416192;2.554880] | 70.108% | 51/51 |
| 2 | 5.725184 [4.649984;6.288384] | 1.641472 [1.412096;2.889728] | 71.329% | 51/51 |
| 3 | 5.703680 [4.497408;7.246848] | 1.536000 [1.420288;2.907136] | 73.070% | 51/51 |

These intervals contain SavePrevious and the complete three repetitions of
Hash/Sort/BuildOffsets/Densities/Integrate, with UAV and transition barriers,
root bindings and necessary queue execution between the start/end timestamps.
There are only two timestamps per full-solver sample; query resolve, CPU
submission/flush/fence wait and correctness readbacks are outside the interval.
Each variant is submitted and flushed before the next one. This is a serialized
solver experiment; it does not measure overlap with the Bistro renderer.

## Attribution, separately instrumented

31 paired frames after the same120-frame warmup; internal timestamps delimit
each stage of each substep. Arithmetic means, summed over all three substeps:

| Stage | B1024, ms | Candidate, ms |
|---|---|---|
| SavePrevious | .019588 | .019126 |
| Hash + entry transitions/UAV barrier | .009778 | .008988 |
| Sort + its UAV barriers | .253919 | .224914 |
| BuildOffsets + key transition | .004724 | .004525 |
| Densities + entry transitions | 2.287616 | .516691 |
| Integrate + entry transitions/final UAV barrier | 2.349882 | .859103 |
| Full interval mean | 4.925506 | 1.633346 |

Densities/Integrate account for94.2% of baseline GPU time in this experiment.
That selected the optimization target. The improvement is consistent with more
coherent bucket traversal across lanes; kernel-level cache/stall attribution was
not captured, so this is not proof of a particular bandwidth bottleneck.
Stage means already belong to the full interval and must not be added to it.

The original Nsight parent Fluid range (4.731826ms mean) and control capture have
different profiler overhead and evolving simulation states. They are attribution
evidence, not baseline/candidate paired timing samples. REGIMES duplicate parent
rows and gr__dispatch_count are not API dispatch counts.

## Correctness and build

- Separate Release fixture build passed in11.85s. Two existing missing
  DirectXTK12 PDB warnings; no compilation/link errors. Initial sandbox build
  failed on Windows SDK access; the authorized retry completed normally.
- Production shader oracle passed with D3D12 debug layer and GPU-based validation
  in7.15s. Both complete particle buffers, density, exact sorted pairs and offsets
  match B1024 byte-for-byte after1, then3, then3 more substeps. Finite values,
  changed velocity and cumulative ping-pong head are checked.
- Sizes3/1023/1025/2049/51975 cover partial dispatch groups and multiple sorting
  tiles; three particles share an identical position to exercise coincident
  fallbacks. Grid neighbors cross group/tile boundaries. Both legacy and
  near-pressure/reference branches run.
- Production LegacyTracked and PlacedUav paths pass. For2049, completed
  DIRECT→COMPUTE→DIRECT runs pass, with flush/readback before queue switches.
  Manual baseline uses B1024. The inherited log's phrase “all tiles” is stale
  for simulation routes; independent sort-oracle tests still cover512/1024/2048
  and the global reference over17 sizes through1048576, repeated min/max keys,
  reverse/random keys, exact pairs/offsets and padding.
- Three ordinary paired runs and the separate stage run compare complete Bistro
  state after warmup/final frame byte-for-byte. No numerical tolerance accepted.
- All runner fingerprints passed. Production shader equals the fingerprinted
  experimental candidate byte-for-byte. Shared C++ and MGPU files were not edited.

## External load and limits

UnrealEditor stayed open. Its recorded CPU delta was .125/.15625/.125 seconds
during the three approximately3-second runs; no cl/link/MSBuild or other test/App
process was observed in their process timelines. Active GPU samples show
2790–2805MHz graphics and11251MHz memory; startup samples include idle clocks.
A subsequent Windows GPU-engine snapshot reported UnrealEditor3D=1%, other
engines0%; it is not a per-process GPU trace covering the measured interval.
The short1Hz samples cannot exclude short competing GPU bursts. Therefore these
are **conditional paired measurements under the recorded desktop load**, not a
claim of an isolated GPU. Scatter is retained, and all153 paired samples favor
the candidate. Goal >=20% solver improvement is exceeded under these conditions.

No candidate whole-App/frame/debt experiment was run in this short ownership
window. The perf coordinator's prechange instrumented `profile-scope-2` run has
24.1768ms CPU frame /16.9792ms GPU interval, physics1743ticks=29.05sim seconds over
1200frames. Ordinary `quiet-1` measured23.9832ms frame without GPU instrumentation.
These runs had no observed compiler processes; that does not establish full
system isolation. Instrumented fixed-work baseline has1200ticks=20sim seconds,18.5262ms frame and
16.9847ms GPU interval with zero start/end debt. Those are context, not A/B proof
for this shader change. No Bistro FPS improvement is claimed. No other physical
GPU, long soak or visual A/B was tested. Actual per-frame Record call counts in
the full App were not instrumented; fixture checks actual CreateFluidSource
size/default settings and explicitly executes one solver invocation/frame.

## Frozen evidence and reproduction

[Summary](2026-11-12-sph-solver-performance/summary.json),
[SHA256 manifest](2026-11-12-sph-solver-performance/manifest.json), raw
`SphPaired1..3.samples.json`, `SphAttribution.samples.json`, runner logs,
process timelines and sensors are retained alongside this report. Original local
artifacts: `Build/SphSolverPerf/`. The frozen `fixture/` contains the exact built
test source/project, original/candidate kernels and source manifest. It derives
from the existing FluidSortGpuTests, extends its oracle and times the same
Record sequence; the production shader is exercised by its production routes.

To reproduce in this checkout, copy the frozen fixture files into
`Build/SphSolverPerf/`, preserving the B1024 baseline shader, then:

```powershell
msbuild Build/SphSolverPerf/SphSolverPerf.vcxproj /t:Build /p:Configuration=Release /p:Platform=x64 /p:BuildProjectReferences=false /m:2 /nr:false
python Build/SphSolverPerf/run.py Recheck --validate
python Build/SphSolverPerf/run.py Repeat --direct --warmup 120 --rounds 51
python Build/SphSolverPerf/run.py Attribution --direct --warmup 120 --rounds 31 --stages
```

The frozen fixture also includes the exact local BistroWater.h selected by its
include during compilation, preserving the seed/settings implementation.
`GDX12FluidSolver.h` is archived as a prerequisite snapshot; the project resolves
that header through the shared `Rendering/Water` include path. Before rebuilding,
compare the current shared header, GDX12Fluid.cpp, original FluidSortGpuTests.cpp
and BistroWater.h hashes with `fixture/source-manifest.json`; the production
shader should instead match the candidate hash above. Shared FluidParameters.hlsli
must also match its recorded hash. Other repository headers, build props and SDKs
remain prerequisites of this checkout. This is not a self-contained portable build.

The diagnostic project references the perf coordinator's frozen accepted Release
libraries. Those large local libraries are not copied into the report. Serialize
runs with the coordinator. The two-line production change and these evidence
paths are frozen for the common reviewer, who owns the iteration's local commit.

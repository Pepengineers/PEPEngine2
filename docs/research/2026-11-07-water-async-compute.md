# Same-frame water async compute

Water-content baseline: `ec89052` (51,975 particles, three authored MGPU substeps). Solver HLSL, coefficients, quality resolution and timestep calculations are unchanged. This workload supersedes the earlier solver-only timing fixture; its 12.8 ms result is not a measurement of this revision.

`Render.Fluid.AsyncCompute=false` is the default. Setting it true requests COMPUTE for `Fluid simulation`; `Fluid surface` and resolve stay DIRECT. The particle flag is independent: each eligible pass checks its own runtime flag, and the pipeline requests the queue when either flag is set. Missing COMPUTE queue and dual-GPU execution preserve the DIRECT path.

Simulation forks before Bistro environment/geometry work. Particle simulation forks after opaque depth. Multiple pending producers share the same COMPUTE queue without an unconditional DIRECT join between them. A DIRECT logical dependency, physical buffer conflict or incompatible shared-texture access waits for the latest COMPUTE fence, which covers every earlier submission on that queue. DEFAULT buffer readers conservatively join because their states are not locked for simultaneous reads. Immutable upload readers may overlap. The graph always joins remaining work before returning its final DIRECT list, including simulation with hidden rendering.

The solver declares both particle ping-pong buffers, density, keys and offsets as read/write, and constants plus pending seed upload as reads. Surface declares particle buffers and render inputs as reads. COMMON handoffs apply to DEFAULT buffers; fixed-state upload/readback heaps retain their layouts. Each frame slot contains separate solver/render constant records so late motion/history updates cannot overwrite in-flight solver inputs. `SavePrevious` remains before pause and zero-dt returns. Seed retirement occurs before collecting physical accesses, after the frame-slot fence; reset/deletion flush the final DIRECT fence covering prior COMPUTE joins.

DOT includes per-producer fork/signal/join boundaries. `AsyncJoins` counts actual joins. `ComputeTimeMS` is the sum of eligible pass durations using separate per-pass query rings; it is not a calibrated cross-queue frame interval. Use the full DIRECT frame timer for comparisons.

## Reproduction

Run commands from the repository root. Build sequentially with MSBuild from Visual Studio 18 Professional. In this environment PDB tooling produced C1902; the successful diagnostic build uses the same `/Z7 /GL-` override for all modules and rows:

```powershell
$env:_CL_ = '/Z7 /GL-'
$msbuild = 'C:/Program Files/Microsoft Visual Studio/18/Professional/MSBuild/Current/Bin/amd64/MSBuild.exe'
$projects = @('Engine/Engine.Core/Engine.Core.vcxproj', 'Engine/Engine.RendererDX12/Engine.RendererDX12.vcxproj', 'Apps/App.Base/App.Base.vcxproj', 'Apps/App/RenderGraphTests.vcxproj', 'Apps/App/SecondGroupTests.vcxproj', 'Apps/App/BistroRenderGraphTests.vcxproj', 'Apps/App/App.vcxproj')
foreach ($project in $projects) {
    & $msbuild $project /t:Rebuild /p:Configuration=Release /p:Platform=x64 /p:PreferredToolArchitecture=x64 /p:BuildProjectReferences=false /m:1 /nr:false
    if ($LASTEXITCODE) { throw "Build failed: $project" }
}
```

`BuildProjectReferences=false` reuses existing third-party libraries. Build prerequisites normally on a fresh checkout. Use Rebuild, not incremental Build: this environment retained stale renderer objects after a base-pass virtual interface change. Renderer, App.Base and all test/application binaries must agree with the new headers; do not link a stale renderer library against the changed graph or render-pass layout.

```powershell
Build/bin/x64/Release/RenderGraphTests/RenderGraphTests.exe
Build/bin/x64/Release/SecondGroupTests/SecondGroupTests.exe --async
Build/bin/x64/Release/SecondGroupTests/SecondGroupTests.exe --async-gpu-validation
Build/bin/x64/Release/BistroRenderGraphTests/BistroRenderGraphTests.exe
Build/bin/x64/Release/BistroRenderGraphTests/BistroRenderGraphTests.exe --matrix-gpu-validation --warmup=1 --samples=2
```

## Timing harness

The executable reads and immediately persists CVars to its runtime copy of INI, not the repository source INI. Correctness tests intentionally exercise other AO implementations, queue flags and upscalers. Restore the runtime config before conditioning/performance, then fingerprint the normalized runtime INI after any deliberate SetFromString conditioning. The root production config is unchanged.

```powershell
Copy-Item Config/Engine.ini Build/bin/x64/Release/BistroRenderGraphTests/Config/Engine.ini -Force
Build/bin/x64/Release/BistroRenderGraphTests/BistroRenderGraphTests.exe --matrix --warmup=64 --samples=128
```

One process executes rows `0,1,2,3,3,2,1,0`. The command above uses 64 warmup and 128 measured frames: 192 frames per row preserve the phase of XeGTAO's 64-frame noise cycle without adding an AO reset API. Defaults remain 24/120 for compatibility; use the explicit 64/128 protocol for the final comparison. Row bits: W=1, P=2. `--row=0..3` selects one combination; `--warmup=N --samples=N` controls run length. The later AO implementation can extend the table using `--matrix-ao` (AO=4); today this intentionally fails because `Render.AO.AsyncCompute` does not exist. No AO implementation, execution-point or normal-source change is included.

All rows enable water simulation/rendering and particle simulation/drawing. The fixture explicitly creates the street basin and grass, verifies one basin/51,975 seed particles and 250,000 enabled-emitter capacity, and aims a fixed camera at the basin. Capacity is not GPU live count: particles can die as the fixed sequence advances. Reset and seed are identical across rows; shader total time and dt are fixed, and wind/grass histories are reset per series. Other scene objects and lighting retain the fixture's source scene. The existing `--timing` remains its old dt=0 particle-only diagnostic and must not be used for water comparisons.

`Build/AsyncWaterMatrix/samples.csv` contains raw full GPU frame time, compute duration sum, CPU frame/submission time, row/order/flags, dt and declared ECS population. CPU time excludes the explicit post-frame drain but includes normal frame-slot waits. Each measured frame drains the queues before reading timers; this is labelled `cpu-drain`, not queued throughput. Per-series DOT files sit beside the CSV. Copy artifacts before another run overwrites them. For fingerprints/timeouts use the repository `tools/agent-skills/peengine-perf/scripts/run_job.py` runner.

## N to N+1 design, not implemented

Both solver ping-pong buffers are written during a simulation frame. Concurrent drawing therefore needs separate immutable published snapshots, with enough slots for graphics frames in flight and a producer slot. COMPUTE waits before reusing state/snapshot storage; DIRECT waits for the consumed snapshot's publication fence. `PreviousPosition` must come from the previously displayed snapshot, not an arbitrary solver substep. Snapshot time/dt, container transforms and reset/settings generation must travel together. Pause/reset/disable/deletion/resize require explicit draining or generation invalidation. Displaying frame N simulation in frame N+1 adds one frame of control latency.

The current final graph join prevents cross-frame overlap. Removing it requires persistent cross-frame dependencies and retirement for both queues, every frame input, upload, descriptor and resource owner. This change retains same-frame results and does not implement that mode.

## Validation results

Release/x64 Engine.Core, RendererDX12, App.Base, App, RenderGraphTests, SecondGroupTests and BistroRenderGraphTests rebuilt successfully with `/Z7 /GL-`. Incremental-build results were discarded after a stale-object ABI failure at `GDX12ParticlesPass::Initialize + 13`; forced Rebuild resolved it. Third-party missing-PDB and compiler-override warnings remain.

- `RenderGraphTests --gpu-validation`: passed (`graph-final.json`), including multiple producers, queued RAW/WAR/WAW, fallback, exception retirement, concurrent textures and fixed-clock/reset/pause/normal-Tick contracts.
- `SecondGroupTests --async`: passed (`water-final.json`).
- `SecondGroupTests --async-gpu-validation`: passed (`water-pixels-gbv.json`), including bit-identical solver states and queued depth/both-motion components across queues, paused and zero-dt controls, seed/reset/deletion and hidden frame-retirement joins. The final repeated suite also passed with alternating DIRECT/COMPUTE flags without resetting the running simulation (`water-toggle-gbv.json`, 36.68 seconds).
- Ordinary `BistroRenderGraphTests`: passed (`bistro.json`), exact HDR across pooling/particle queue toggles, both AO placements/implementations, odd resize and Native/XeSS.
- `--matrix-gpu-validation --warmup=1 --samples=2`: passed in 855.98 seconds (`matrix-gbv.json`), all eight forward/reverse series and the combined-async pooling/queued/odd-resize/Native-XeSS tail. 16 measured validation samples and eight DOT files are archived in `Build/AsyncWaterValidation/matrix-gbv-artifacts/`. Actual basin seed count is 51,975 and enabled-emitter capacity is 250,000 throughout. These samples include GBV overhead and are not a performance comparison. Runtime AO in this correctness run was implementation 3/quality 2, left by the preceding exhaustive AO checks; the source INI selects implementation 2/quality 2 for the final performance protocol.

Observed combined DOT example: water DIRECT fork 169 / COMPUTE signal 11, particle fork 170 / signal 12, both joining before `Fluid surface`. No intermediate DIRECT join precedes the second async producer.

Logs and runner receipts are under `Build/AsyncWaterValidation/`. Root `Config/Engine.ini` and solver HLSL fingerprints were unchanged during the matrix. The runtime INI is intentionally mutable and must be restored/normalized before performance. Long performance matrices, GPU Trace overlap analysis, Debug builds, dual-GPU hardware and other GPUs are outside this correctness run; no speedup is claimed.

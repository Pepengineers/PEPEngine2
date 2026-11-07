# Same-frame AO async compute

Baseline: `bd994438a66ebcf7bccf459293193f9e080f41c4`. This change adds `Render.AO.AsyncCompute=false` independently of W/P. `IsAmbientOcclusionAsyncComputeEnabled()` deliberately lives outside the 23-scalar `GDX12AOSettings` snapshot: changing queues does not change settings or reset temporal history. Missing COMPUTE queue and the existing dual-GPU guard use DIRECT.

The default remains late XeGTAO (`Implementation=2`, `Quality=2`, `ExecutionPoint=1`) using current G-buffer normals. SSAO, HBAO, XeGTAO, CACAO and neutral output use the same kernels and parameters on both queues. No AO or water HLSL, water seed or MGPU timestep is changed.

## States, ownership and retirement

AO declares read-only depth/normals, upload constants/camera and an explicit UAV output. DIRECT publishes read unions and releases the output to COMMON; COMPUTE transitions the output to UAV and releases it back to COMMON before signalling. The first DIRECT consumer waits before transitioning/reading it. Only explicit UAV texture writes are supported. Async outputs and their fallback sets retain conservative whole-graph texture lifetimes, so linear pooling cannot alias across queues.

AO's private scratch resources use NON_PIXEL_SHADER_RESOURCE on both paths; a subsequent COMPUTE frame never inherits an internal PIXEL state from DIRECT. Inputs preserve a previously published shared-read union. CACAO already uses COMPUTE-compatible internal texture states, except its final output transition: the integration now selects NON_PIXEL_SHADER_RESOURCE for a COMPUTE command list and retains GENERIC_READ for DIRECT, followed by engine-side normalization. Its context, constant ring and size-dependent resources remain pass-owned. Final DIRECT fences cover their COMPUTE users before frame-slot reuse, resize or destruction.

The scheduler retains one record per pending producer, including fence and physical texture/buffer accesses. A consumer waits for the fence of the last producer it actually needs and retires that ordered prefix. Later producers and shared-read owners remain pending. A shared read lock ends only when no remaining producer owns that resource. Buffer and texture RAW/WAR/WAW, unmanaged or incompatible reads, physical aliases, recording failure and final retirement remain explicit boundaries. DEFAULT buffer read/read remains conservative unless both readers explicitly declare CBV state. AO, water and particle simulation/render declare their shared CameraCB reads. The existing resource read lock now accepts CBV state for buffers, publishes it on DIRECT and prevents COMMON release until the last pending reader retires. Immutable upload reads can overlap. A COMPUTE texture reader after a pending writer conservatively joins before DIRECT publishes its shared layout. Same-queue UAV writers use queue order.

Water surface now declares its real SRV read states and publishes its own depth/motion targets in PrepareFrame when rendering. The graph therefore does not mistake dynamic output writes for writes to the opaque fallback before Execute. First frame/reset and disabled fallback are checked before graph execution. This allows the AO depth reader to overlap surface raster/filter work. Fluid resolve remains unmanaged because it copies its background through COPY_SOURCE. The new scheduling/barrier contracts also affect W/P-only rows; compare all eight combinations on this revision rather than combining earlier W/P measurements with new AO rows.

Observed late triple-async sequence in `series-7.dot`: W fork/signal 154/10 retires before `Fluid surface`; P 155/11 and A 156/12 retire before `Clustered deferred lighting`. No join retires those later readers at surface. This is a scheduling window, not a calibrated GPU-overlap or speedup measurement.

Late triple-async sequence: W forks before geometry, P after depth, A after G-buffer; surface waits for W while later P/A depth readers remain owned; lighting waits for AO and the preceding COMPUTE prefix. Final retirement still joins any unconsumed output. When water is absent/hidden, its dynamic outputs still declare a conservative opaque-fallback write, so that path can join earlier at surface. The active-water matrix is covered by the late-join assertion. No cross-frame water mode is introduced.

## Build and checks

Run from `D:/Project/PEPEngine2`. A sequential Release rebuild keeps renderer, App.Base and callers consistent. This environment requires the known `/Z7 /GL-` PDB-tooling workaround:

```powershell
$env:_CL_ = '/Z7 /GL-'
$msbuild = 'C:/Program Files/Microsoft Visual Studio/18/Professional/MSBuild/Current/Bin/amd64/MSBuild.exe'
$projects = @('Engine/Engine.RendererDX12/Engine.RendererDX12.vcxproj', 'Apps/App.Base/App.Base.vcxproj', 'Apps/App/RenderGraphTests.vcxproj', 'Apps/App/AOTests.vcxproj', 'Apps/App/SecondGroupTests.vcxproj', 'Apps/App/BistroRenderGraphTests.vcxproj', 'Apps/App/App.vcxproj')
foreach ($project in $projects) {
    & $msbuild $project /t:Rebuild /p:Configuration=Release /p:Platform=x64 /p:PreferredToolArchitecture=x64 /p:BuildProjectReferences=false /m:1 /nr:false
    if ($LASTEXITCODE) { throw "Build failed: $project" }
}
```

Restore source INI into each test's `Build/bin/x64/Release/<name>/Config/Engine.ini` before starting it. Runtime SetFromString persists immediately; the repository source config is not the live file.

```powershell
Build/bin/x64/Release/RenderGraphTests/RenderGraphTests.exe --gpu-validation
Build/bin/x64/Release/AOTests/AOTests.exe --gpu-validation
Build/bin/x64/Release/BistroRenderGraphTests/BistroRenderGraphTests.exe --ao-placements
Build/bin/x64/Release/BistroRenderGraphTests/BistroRenderGraphTests.exe --matrix-gpu-validation --matrix-ao --warmup=0 --samples=1
```

`--ao-placements` checks actual early/late Bistro bindings and all four implementations through DIRECT/COMPUTE/DIRECT. `--ao-placements-gpu-validation` additionally enables GBV. AOTests checks output identity, settings/history and neutral controls with fixed noise, separately from the full-scene matrix.

## Performance handoff

```powershell
Copy-Item Config/Engine.ini Build/bin/x64/Release/BistroRenderGraphTests/Config/Engine.ini -Force
Build/bin/x64/Release/BistroRenderGraphTests/BistroRenderGraphTests.exe --matrix --matrix-ao --warmup=64 --samples=128
```

After restoring INI, the caller owns conditioning and normalized runtime-config fingerprints. W=1, P=2, A=4; `--row=0..7` selects a row. All eight combinations execute forward/reverse in one process. The existing CSV schema, fixed dt=1/60, camera/reset/seed, 51,975 water particles and 250,000 emitter capacity are retained; capacity is not live count. Every W/P/A performance row keeps all three techniques enabled, with late XeGTAO quality 2. 64+128 frames preserve XeGTAO's 64-frame noise phase. Raw CSV and per-series DOT remain in `Build/AsyncWaterMatrix/` and are overwritten by another run. These are labelled CPU-drain samples, not queued throughput. Validation timings must not be treated as performance evidence.

## Results and limits

Release builds passed. RenderGraphTests with GBV passed prefix consumers, two UAV writers, retained readers, physical RAW/WAR/WAW aliases, pooling exclusion, final/exception retirement and DIRECT fallback. AOTests with GBV passed all four implementations, neutral path and exact DIRECT/COMPUTE/DIRECT output identity without queue-induced history resets. Actual Bistro early/late bindings and toggles passed all implementations with the debug layer (`bistro-placements-final.json`, 23.07 s). The ordinary Bistro HDR/pooling/placement/resize/XeSS regression also passed on the final implementation (`bistro-regression.json`, 27.67 s). A new DEFAULT CBV owner-retention fixture passed GBV; particle simulation/render camera declarations and water prepublished-target/reset/disable behavior passed SecondGroupTests with GBV. Neutral AO without a camera buffer also passed.

Final targeted receipts are `graph-cbv-gbv.json` (1.82 s), `ao-final-gbv.json` (15.64 s, including missing-camera neutral output), and `water-targets-gbv.json` (40.08 s, first-frame/reset/disable physical output identities and the particle suite). The earlier fully conservative matrix passed, but its DOT exposed an early join. `matrix-final-gbv.json` then deliberately failed the new late-join assertion, exposing the fallback-as-write identity issue; it is not accepted evidence for the final window. The published-target matrix `matrix-published-gbv.json` passed all 16 rows and the triple-async pooling/queued/odd-resize/Native-XeSS tail in 708.80 seconds. All fingerprints passed. Its 16 validation samples and 16 DOT files are archived in `Build/AsyncAOValidation/matrix-published-artifacts/`; they include GBV overhead and are not performance evidence. Receipts and logs are under `Build/AsyncAOValidation/`. Long performance series, GPU Trace attribution, Debug builds, another GPU and dual-GPU hardware are not part of this correctness run. No AO speedup is claimed.

# AO validation — 2026-11-01

> Handoff status: **UNVERIFIED**. The user stopped all validation on 2026-11-01.
> The runs below are historical and precede the last native CACAO load-counter
> UAV-barrier change. They are not acceptance of the final revision. The main
> chat owns final build, tests, GPU checks, review and simplification. No further
> checks were started after that instruction.

Copy: `D:/Project/PEPEngine2/Build/Features/AO`, branch `codex/feature-ao`, starting
HEAD `271551e`. Original warning WIP was preserved; unrelated files are excluded
from the AO commits. No old implementation commits were restored.

The code uses one AO pass and the existing texture, descriptor, upload-buffer,
shader compiler and CVar interfaces. There is no new manager, render graph,
registry, scene or demo entity. Official XeGTAO/CACAO sources are pinned and their
small adaptations documented in their dependency directories. Settings and the
shared HDR integration seam are described in [ambient-occlusion.md](ambient-occlusion.md).

## Reproduction

Run from the copy above. VS 18 Professional MSBuild, Windows SDK 10.0.26100.0,
MSVC 14.51.36231, Release/x64, PreferredToolArchitecture=x64:

```powershell
$env:_CL_ = '/Z7 /GL-'
$msbuild = 'C:/Program Files/Microsoft Visual Studio/18/Professional/MSBuild/Current/Bin/MSBuild.exe'
& $msbuild Apps/App/App.vcxproj /p:Configuration=Release /p:Platform=x64 /p:PreferredToolArchitecture=x64 /m:1 /nr:false
& $msbuild Apps/App/AOTests.vcxproj /p:Configuration=Release /p:Platform=x64 /p:PreferredToolArchitecture=x64 /m:1 /nr:false
& $msbuild Apps/App/LightingGpuTests.vcxproj /p:Configuration=Release /p:Platform=x64 /p:PreferredToolArchitecture=x64 /m:1 /nr:false
Build/bin/x64/Release/AOTests/AOTests.exe
Build/bin/x64/Release/AOTests/AOTests.exe --gpu-validation
```

`AOTests` holds `Local\PEPEngine2FeatureGpuValidation` for its process lifetime.
Wrap `LightingGpuTests` and Bistro runs with the same mutex:

```powershell
$gpuMutex = [System.Threading.Mutex]::new($false, 'Local\PEPEngine2FeatureGpuValidation')
try {
    $null = $gpuMutex.WaitOne()
    & Build/bin/x64/Release/LightingGpuTests/LightingGpuTests.exe
} finally {
    $gpuMutex.ReleaseMutex()
    $gpuMutex.Dispose()
}
```

The test projects generate their own minimal `Config/Engine.ini` because they do
not link the application's unrelated Jobs/Loading setting owners. AO INI loading
is also tested independently before startup closes. The application still uses
the complete repository configuration. Its existing INI is not overwritten by
ordinary MSBuild; copy the updated template when testing an old output directory.

The local `/Z7 /GL-` override avoids the known sandbox PDB/toolchain mismatch;
project-wide compiler settings were not changed. Existing conversion warnings,
duplicate property-sheet import warnings and missing third-party PDB warnings
remain. None prevents these targets from building.

## Checks

- Application and both focused targets build successfully; CACAO's 33 DXIL
  entry points are generated from the pinned source during the renderer build.
- CPU CVar tests cover INI registration, runtime flags/writes, enum/range/finite
  rejection, preset inheritance, preserved manual overrides for all work counts,
  and safe fade normalization.
- GPU readback covers all four algorithms on a plane, a 50 cm depth step at
  450/500 cm, perpendicular planes, background and geometry at 100000 cm beyond
  the fade end. Every output is finite and in [0,1]. Off, Enabled=0, Intensity=0,
  background and fully faded geometry are exactly one.
- Occlusion at the step/corner is compared with the matching region of the flat
  plane, so an algorithm's baseline angular error cannot satisfy the test.
- Denoise, Low/Mid/High, all sampling overrides, radius/sharpness/bias, XeGTAO
  falloff/thin-occluder controls, and CACAO native quality/power/clamp/detail/
  adaptive budget change actual GPU output. Detail AO is tested on shallow
  two-pixel grooves; the native algorithm deliberately suppresses it across
  large depth discontinuities. Native qualities 0,1,2,3,4 and blur counts 0/2/8
  are exercised.
- Three queued frame slots with different intensities are copied before the next
  dispatch overwrites the shared output, then compared against isolated GPU
  results. All four implementations match within 1e-5, including CACAO's upload
  ring. Switching implementations, disabling/re-enabling and repeated native
  context use are covered.
- Resize and quality switches run at 17x13, 1x1, 321x241 and 1280x720. 1x1 uses
  the documented neutral sub-icon path. Odd native CACAO input loads are clamped
  at the last incomplete 2x2 quad; the former dark plane border is fixed.
- Regular DX12 debug validation and a separate GPU-based validation run cover
  the same resources and native dispatches. Diagnostics are checked after the
  pre-existing empty-scene cache setup; no AO/HDR warnings or errors are accepted.
- The real WBOIT→XeSS→tone-map pipeline preserves HDR `(3,2,1)` before display
  conversion and produces approximately `(225,213,188)` in the UNORM backbuffer.
  First-frame/reset consumption and an odd 129x97 resize are checked on the GPU.
- The lighting fixture renders through existing GPU culling/ExecuteIndirect,
  deferred PBR and WBOIT. Forced visibility 0/1 reduces diffuse IBL while
  preserving local direct-light energy (absolute tolerance 0.01 HDR), specular,
  emissive and baked color. Existing cubemap, IBL, point/spot shadow and glass
  lighting tests also pass. Direct shadow energy was `0 / 399.315` after forcing
  occlusion.
- The existing `PBRMathTests.cpp` passed normalization, reciprocity, energy,
  grazing-angle and sRGB assertions using the shared shader math.

## Image comparison and angular tolerance

![Synthetic AO visibility](images/ao-fixtures.png)

These are synthetic readbacks, not Bistro screenshots. White is one. Radius is
150 cm and vertical FOV 90 degrees to expose edge behavior. SSAO, HBAO and CACAO
return exactly one over the unoccluded plane. Mid XeGTAO returns a minimum about
0.753 and mean 0.921, with the center above 0.98. This is the official algorithm's
projected-normal angular approximation, not depth-buffer self-shadowing or a
color-space multiply. A separate numerical evaluation of an unoccluded oblique
slice at view direction proportional to `(1.3,1,-1)`, including Intel's 0.05
projected-length correction and final power 2.2, converges to about 0.749 with
many slices. The test therefore requires full-frame XeGTAO visibility above
0.70, center above 0.98 and additional step/corner occlusion relative to the
same plane. Other algorithms use 0.96 full-frame tolerance. The limitation is
visible in the comparison and is not hidden by post-normalizing Intel's output.

Measured Mid step minima: SSAO 0.826, HBAO 0.729, XeGTAO 0.553, CACAO 0.661.
Corner minima: 0.669, 0.681, 0.380, 0.577 respectively. Equal radius/intensity
does not give equal darkness because the vendor algorithms use their own
falloff, strength and power calibration.

## Cost

NVIDIA GeForce RTX 3060 Laptop GPU, 5994 MiB dedicated VRAM. Same 1280x720 depth
step, radius 150 cm, static noise, no upscaler in the timed AO workload. Mean of
10 warmed GPU timestamp intervals per cell, regular debug layer enabled,
GPU-based validation disabled for timing. The interval includes normal
preparation, algorithm, depth preparation/mips where applicable, denoise,
resolve and their barriers; it excludes input upload/readback and lighting.

| AO pass | Low, ms | Mid, ms | High, ms |
| --- | ---: | ---: | ---: |
| SSAO | 0.437 | 0.692 | 1.072 |
| HBAO | 0.700 | 1.371 | 2.257 |
| XeGTAO | 0.321 | 0.415 | 0.579 |
| CACAO | 0.374 | 0.448 | 0.530 |

DXGI reports approximately 88.8 MiB of process-local VRAM for the fixture,
including its descriptor heaps and inputs; first CACAO use at this size raises
that to about 99.0 MiB. Native CACAO size-dependent resources add about 10.25 MiB.
The engine's shared AO textures reserve about 25 MiB at 1280x720 (23 bytes/pixel
plus five FP32 depth mips). They are shared across frame slots. Preset changes
alter work counts, not texture resolution/allocation. These are isolated AO
measurements, not total Bistro VRAM or an application FPS promise.

## Bistro and limits

The actual Release application was started four times with Implementation 0/1/2/3
through its `Engine.ini`, under the GPU mutex. Each loaded 309 textures, rendered
its benchmark frame and exited 0. First-ready times were approximately 41.62,
38.78, 37.40 and 38.80 seconds; these include scene loading and are not AO timings.
Reports are `Build/bistro-ao-0.json` through `Build/bistro-ao-3.json`.

A live-window visual inspection was attempted, but Computer Use returned
`app approval timed out`; no Bistro screenshot or visual-approval claim is made.
The raw AO image above remains explicitly synthetic.

No long-run stability claim, 60 FPS target, multi-GPU validation or comprehensive
moving-camera temporal/artistic assessment is made. The three-slot fixture and
native resize/history tests are separate from those untested areas. CACAO's first
selection creates native pipelines/resources and can hitch. Viewports smaller
than 16 on both axes intentionally return neutral AO. HDR seam changes should be
merged once with other feature branches, not layered as another tone-map pass.

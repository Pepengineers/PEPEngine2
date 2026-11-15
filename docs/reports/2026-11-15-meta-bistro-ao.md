# Bistro AO in MetaRenderGraph

## Implementation

Ordinary Bistro now creates and freezes the existing `SceneAO` owner in each
frame slot, using the shared `GDX12AmbientOcclusionPass` pipelines and resources.
Depth prepass and GBuffer precede AO; deferred lighting declares a read of the
real AO output and binds it at `t14`. The opaque shader variant passes visibility
to the same `ShadeSunAndIBL` function as App: only diffuse IBL is occluded.
Sun/local direct lighting, specular IBL, emissive and baked surfaces retain their
existing semantics. HDR display transform remains the output path.

`Render.AO.Enabled=false` uses the existing neutral-output dispatch (visibility 1);
the AO algorithm and denoise packets are not appended. No new AO implementation,
UI, scheduler or settings API was introduced. Ordinary `PrepareFrame` continues
to force `AOAfterDepthPrepass=false`, so early AO and legacy scene preparation
remain excluded. The existing Meta queue placement is unchanged.

Slot-owned depth, normals and full-resolution AO have matching extents. Existing
AO resize and history-reset handling is reused; graph resource accesses provide
producer/consumer dependencies, transitions and UAV ordering. There are no new
Flush calls, readbacks or scene copies.

## Effective startup settings

The source `Configs/Startup.ini` explicitly selects Enabled=true,
Implementation=2 (XeGTAO), Quality=2 (Mid), Resolution=0 (Full),
ExecutionPoint=1 (BeforeLighting), RadiusCm=50, Intensity=1 and
DenoisePasses=-1 (preset inheritance, **2 effective passes**).
Unchanged shared defaults resolve to **2 slices**, **2 steps per half slice**,
Sharpness=0.98, Falloff=0.615, ThinOccluder=0,
FadeStartCm=5000 and FadeEndCm=15000. Xe denoise blur beta is 0.2+0.98=1.18.
The native startup render grid remains 1280x800; AO uses the actual depth extent.

Camera, scene, authored lights, shadow cache/journals, primary adapter 0, paused
simulation and native reconstruction settings were not edited. Transparency,
probes, SSR, clouds, grass, water, particles, skinning, post effects, upscaling
and frame generation remain excluded from the ordinary path.

## Verification

Release x64 build passed through Visual Studio's `VsDevCmd.bat`, outside the
sandbox. The first two sandboxed attempts failed with LNK1101/C1902 (PDB manager
version/access errors); no project/toolset changes were needed. The successful
build retained external-library LNK4099 missing-PDB warnings.

One ordinary no-argument launch on 2026-11-15 displayed the Bistro entrance,
opaque materials, shadows and the integrated AO lighting result. The window
rendered live frames; no obvious visual failure or current-run ERROR/FATAL was
observed. Closing the window completed resource shutdown normally. This confirms
GPU rendering, not a measured performance or AO accuracy comparison.

Both operation dumps include GBuffer followed by `AO prepare normals/depth`,
`XeGTAO depth pyramid`, `XeGTAO main`, two `XeGTAO denoise` packets and `AO resolve`,
then the existing GPU light grid, deferred lighting and HDR display transform.
No excluded technique was added to the executing list. The shared AO owner
initializes shader pipelines for its other implementations, but these algorithms
do not execute in the selected XeGTAO path.

Build/run logs, operation dumps and the independent source-review receipt are
under `Build/Verification/meta-bistro-ao/`. Source review found no actionable
issues. `git diff --check` passed. The disabled neutral path and resize ownership
were checked in source; they were not exercised in additional runs.

Existing `AOTests.cpp` settings/algorithm coverage was inspected; no tests were
added, changed or run, as required by this stage's verification contract.
No `--smoke`, quality matrix, precision checks, long run or profiling is used.
AO image quality, FPS and CPU/GPU cost remain subject to user acceptance; no
performance improvement or measured AO cost is claimed.

The pre-existing changes in `Apps/App.Base/src/SceneManagerModule.cpp`,
`Apps/App/tests/BistroAdditionsTests.cpp` and `Temp/` are excluded from this task
and commit. The working build includes the existing scene edits and therefore
does not establish a controlled comparison with older lighting configurations.

Executable: `D:/Project/PEPEngine2/Build/bin/x64/Release/App.MetaRenderGraph/App.MetaRenderGraph.exe`.
Stop at this AO stage until the user accepts it; transparency is separate work.

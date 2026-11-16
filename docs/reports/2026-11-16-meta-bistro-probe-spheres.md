# Bistro interior cubemap spheres

Implemented on `codex/bistro-rendering`, live baseline `4a284b38` (rather than the
task preparation baseline `5e836132`). Existing water/shadow commits and unrelated
`BistroAdditionsTests.cpp`, `Temp/` and animation task edits were preserved.

## Placement and controls

Ordinary Meta reuses the generated sphere and only the last ten interior anchors
in `BistroReflections::Tables`. App's legacy all-table behavior is retained.
Seed=1, Count=4, reset counter=0 selects these unique one-based source anchors:

| Table | Centre/capture origin cm | Radius cm |
| --- | --- | --- |
| 18 | 790.023, 164.327, 76.496 | 24 |
| 16 | 501.667, 164.547, 192.991 | 24 |
| 17 | 779.009, 164.541, 302.068 | 24 |
| 14 | 173.030, 164.327, -648.631 | 24 |

The centre is 40 cm above the original tabletop, leaving 16 cm clearance below
the sphere. There are no terrace anchors or physics bodies. Entity order is
sorted consistently with published cube indices; count/seed/reset recreates the
selection. The fixed original anchors never follow future furniture physics.

`App.ReflectionProbes.Count`: Meta 0..10, App 0..21; `Seed=1`, `ShowSpheres=true`,
`ShowBounds=false`, `Resolution=128`, `PrefilterSamples=64`, `Implementation=2`.
Increment `App.ReflectionProbes.Recapture` for explicit recapture, `Reset` to
recreate anchors. Existing `Render.Reflection.*` controls remain in use.
Initial camera: (400,230,-370) cm, pitch 8/yaw 40 degrees, free camera retained.

## Capture and publication

SceneReflections receives explicit local-shadow inputs from SceneLighting owners.
Local capture shadows cover all faces, independently of camera receiver masks;
a separate static-only atlas excludes dynamic casters for static capture/bake.
Each owner has a CPU DSV heap sized to the existing per-slot atlas capacity.
Probe sun shadows use the scene bounds, independently of camera CSM coverage.

Mode 0 captures current permitted geometry by existing face budget. Mode 1 copies
static color AND depth before drawing current dynamic geometry with depth testing.
Sun/environment changes invalidate that static lighting cache and require new
static bake work; the day cycle remains enabled. Frozen mode 2 ignores camera,
sun/environment and dynamic changes, but recaptures on explicit request, static
geometry/material/membership changes, quality or resource changes. Partial frozen
capture is discarded when static geometry changes, ensuring a new generation is
not lost behind the completed-frozen scheduling guard.

Preview materials are excluded from capture/shadow lists. Existing transparency
and particle capture exclusions remain. Dynamic ownership remains independent of
sleep or movement; animated painting UI is already ReflectionDynamic=true.
Physics integration is a separate stage.

Initial/topology collection uses the existing snapshot builder; steady updates
map changed instances/transforms/materials to cached primitives without traversing
all static renderers. The snapshot reads the dirty journal before local shadows
consume it. Sleeping unchanged objects produce no new instance work.

Capture -> prefilter -> publication copy -> metadata upload -> preview are explicit
Meta dependencies. Only six completed faces and all required mips publish; metadata
marks readiness and logical mip zero. Pending probes use neutral grey or a previous
complete publication. Existing Submitted/fences and resource retirement remain.
Ordinary capture skips diagnostic evidence/readback allocation and packets, private
submits and steady Flush. Resource replacement retains the existing rare drain.
Dynamic publication does not reset the entire temporal history every frame.

## Preview composition

The dedicated HDR draw is after opaque lighting, SSR and opaque water, before
mesh transparency/particles and postprocessing. PBR/SSR retain static global IBL.
It samples reflect(view,normal), logical mip 0, without BRDF/Fresnel/AO/lighting.
Opaque aerial perspective runs before the preview; WBOIT does not apply it again.

Per-slot preview depth/motion copy water-composited inputs, then spheres depth-test
and write reverse-Z depth and transform/camera motion. Walls occlude spheres and
transparency/particle raster/post consumers use updated depth. Particle collision
retains original opaque depth. Newly appearing previews start with zero motion;
subsequent frames use previous displayed transforms and camera matrices.

## Verification

- Release build passed; existing external PDB/conversion warnings remain.
- DXC compiled ProbePreview VS/PS, ProbePreviewBackground PS and ordinary Culling CS.
- Ordinary smoke passed: GPU image presented, three spheres visibly reflected the
  room above interior tables; UI paintings and cloud image remained visible. The
  fourth anchor is outside the initial view. No exterior spheres exist by selection.
- First failed startup exposed an upload-state declaration; corrected to GENERIC_READ.
  Another failed startup exhausted the shared DSV heap; dedicated owner heaps fixed it.
- Source review connected the ShowBounds UI control and corrected DSV heap/slot
  destruction order. Final source review corrected partial frozen invalidation and removed redundant
  anchor shuffling. Final build was repeated. The final ordinary launch was stopped by the user with physical Escape.
  Corrected DSV teardown therefore remains unverified at runtime; the previous
  presentation alone did not establish clean resource release.
- Runtime mode matrix/manual recapture, profiling, accuracy tests and long runs
  were NOT RUN, per AGENTS/task contract. Those paths are source-reviewed only.
- The visible title showed roughly 14–16 FPS during the brief ordinary check;
  this is an incidental reading, not a benchmark or CPU/GPU attribution.

Executable: `Build/bin/x64/Release/App.MetaRenderGraph/App.MetaRenderGraph.exe`.
Build log: `Build/meta-bistro-probe-spheres-build.log`; smoke evidence:
`Build/Verification/meta-bistro-probe-spheres/smoke.log`.

Stop for the user's FPS/CPU/GPU acceptance. Interior physics is not started;
its task explicitly begins after acceptance of these spheres. No push.

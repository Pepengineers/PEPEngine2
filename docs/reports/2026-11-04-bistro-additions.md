# Bistro scene additions — 2026-11-04

Correction after visual feedback: the original visibility checks did not detect
inverted soldier winding. The follow-up converter/import regression now checks
surface orientation; the converter reverses M3D triangles for the engine's CCW
rasterizer. The current scene also places probe visualizations above terrace
tables and exposes grass/particle authoring controls. See the current App README.

## Follow-up: previews and authoring controls

The first four probes use actual terrace-table anchors and cubic influence
bounds with half-size 60.55..62.48 cm. Their bottom blend region intersects the
tabletop by 5 cm. `App.ReflectionProbes.ShowBounds` draws clipped colored boxes
and labels; `App.ReflectionProbes.ShowSpheres` draws mirror spheres of radius
0.8 times the box half-size. Spheres default to on; boxes/labels default to off.
The switches are independent.
The preview is a forward unlit mesh pass immediately after deferred lighting,
before SSR and transparency. Each probe entity owns a regular static sphere mesh
and a material selecting its published cubemap slice and first populated mip.
All spheres share one DirectXTK sphere mesh with the engine's CCW winding. The
pass reuses the opaque mesh vertex shader and writes HDR, reverse-Z depth, motion,
normals and the existing baked-material marker to preserve unlit color through SSR.
There is no analytic sphere intersection or fullscreen triangle. An unpublished
map produces an opaque black placeholder. Preview materials are excluded from
ordinary opaque culling, captures and all shadow culling, and from the CPU capture
revision. Visibility toggles preserve the existing mesh instances.

Grass now exposes bounds, density/budget, blade geometry, shadow and wind solver
settings through application CVars. See [grass controls](../rendering/bistro-grass-controls.md).
Particles expose an exact total budget, bounded emitter-local volume and analytic
drag/semi-implicit Euler selection. See [particle controls](../rendering/bistro-particle-controls.md).

The soldier fix changes triangle winding only; UVs, textures and animation data
are unchanged. The dependency-free orientation regression checks every material
section against authored normals (over 99.8% outward faces), and the App checks
the actual imported sections as well. `Take1` also turns the body approximately
171 degrees relative to bind pose; human route yaw adds 180 degrees so they walk
forward. The heading regression uses the actual imported body-root pose and
route displacement. Fox orientation is unchanged.

Follow-up Release/x64 builds use `/Z7 /GL-` and
`/p:WholeProgramOptimization=false`; the renderer was rebuilt to avoid mixing
stale LTCG objects with this local compiler installation. RendererDX12 and
App.Base were rebuilt together for the CPU probe/emitter layout changes.
`ReflectionProbeTests` passed with GPU-based validation, including independent
exact-slice/mip, unlit radiance and unpublished-material checks.
The existing static publication, rebake, runtime mode changes and HDR/XeSS checks
also passed. Logs for this follow-up are under `Build/BistroControls`.
`SecondGroupTests --gpu-validation` passed the skinning, wind/grass, fluid and
particle regressions without debug-layer errors. New particle readbacks matched
independent 0.5/1 cm integrator oracles, rotated-domain reflection and pause.
`BistroAdditionsTests` passed the real-scene defaults/zero/changed/restored
populations, imported soldier surfaces, animation/pause, new grass/particle
controls and clipped box overlay. Sphere previews changed 22147 HDR pixels by
more than 1%; switching them off restored scene RGB within 0.000838 relative
FP16 drift in the original controls run. Its pixel contributions were grass 57090, water 15788, human 48446
and fox 31266, with no debug-layer errors. The terrace spheres and both sides of
the animated human were visually inspected from the HDR readbacks under
`Build/BistroAdditions`.
`BistroStartupTests` also passed: 90 frames, original props/effects, particle
budget compatibility, focus/minimize/resize, enum discovery, F1 and input capture.
The final App/Additions builds and scene run include the NPC heading correction;
its imported-skin-palette regression failed before that correction and passed after it.

The forward mesh implementation passed `ReflectionProbeTests --debug` and full
GPU-based validation, including exact cubemap slice/mip, unlit radiance, opaque
unpublished material, actual WBOIT front/behind/outside occlusion, HDR/XeSS, odd
resize and queued publication. The full Bistro run passed counts 0/2/4/8, actual
mesh-component ownership, visibility toggling, original effects and animation,
with no debug-layer errors. Mesh previews changed 22187 HDR pixels; off restored
RGB within 0.001706 relative FP16 drift. The verified image is
`Build/BistroAdditions/probe-spheres-forward.png`. Release/x64 RendererDX12,
App.Base, App, ReflectionProbeTests and BistroAdditionsTests builds passed.
Logs: `Build/BistroControls/forward-preview-debug-run.log`,
`forward-preview-gbv-run.log` and `forward-preview-bistro-run.log`.
The rebuilt `BistroStartupTests` also passed its 90-frame scene/effects/UI checks
and focus/minimize/resize regression (`Build/BistroControls/forward-startup-run.log`).

## Original integration

Four separate development tasks are retained under
[`docs/tasks/bistro-additions`](../tasks/bistro-additions/README.md).
The App integrates the existing renderer implementations; it retains both Bistro
scene parts, the 346 moving props, eight particle emitters and entrance camera.

| Application CVar | Default | Accepted count |
|---|---:|---:|
| `App.ReflectionProbes.Count` | 4 | 0..32 |
| `App.Grass.Count` | 1 | 0..16 |
| `App.Water.Count` | 1 | 0..8 |
| `App.NPC.Count` | 4 | 0..16 |
| `App.Fox.Count` | 1 | 0..8 |

Counts change at runtime, with zero removing the owned objects. Use the settings
controls (clear their initial search) or the App executable's own INI. These
application variables are registered only by Bistro and tests that include it.

The four 128-pixel reflection probes use `Implementation=2`: capture static
geometry/environment once, filter and publish, then stop capture/shadow/filter
work. Dynamic mode and cached-static-plus-dynamic mode retain their behavior.
Frozen captures exclude moving props, characters, grass, particles and water.
Explicit invalidation, capture-setting or topology changes rebuild the snapshot.
This is an in-memory initial bake, without a persistent disk cache.

Grass has 1232 blades and an analytic wind field per patch. Its first position is
`(-420,23.3,150)` cm, above the OBJ pavement at y=23.162 cm. Water has a shared
512-particle seed and a five-box open basin at `(-410,41.3,-100)` cm. Five ground
raycasts and an Exterior triangle/AABB intersection check confirmed flat support
and clearance from the original scooter and street posts.

Humans share the converted MGPU soldier scene and `Take1` animation; the interior
fox uses the existing Khronos Fox `Walk` clip. The reproducible, dependency-free
converter and source hashes/provenance are retained with the asset. Human route
footprints were checked at 80 samples on pavement; 16 fox route centers were
checked on the interior floor at y=39.622 cm. Routes use stable independent phases.

## Verification

Release/x64 builds use VS 2026 v145 and `_CL_=/Z7 /GL-`, avoiding the local PDB
manager mismatch. Existing C4267, third-party CACAO encoding, override and LNK4099
warnings remain; no warning-free build is claimed.

`App`, `BistroAdditionsTests`, `ReflectionProbeTests` and `BistroStartupTests`
Release/x64 builds passed. The original startup regression also passed: both
scene parts, 346 props, entrance camera, 90 rendered frames, runtime particle
budgets, focus/minimize/resize and the ImGui controls/input checks.

`BistroAdditionsTests` loads the real App and checks default, zero, changed and
restored populations, invalid counts, shared resources, character transforms and
changing skeletal poses, character pause, original props/emitters and finite HDR.
Independent feature toggles changed these final-view HDR pixel counts:

| Feature | Changed pixels |
|---|---:|
| Grass | 57090 |
| Water | 15788 |
| Human NPC | 47646 |
| Fox | 32223 |

The final integrated run passed without D3D12 debug-layer errors. Its first run
exposed an opaque-depth state hazard between water resolve and front transparency.
WBOIT now transitions depth to graphics SRV/read-only-depth state before binding
it; descriptor flags were not relaxed to hide the error.

Reflection `--cpu`, `--debug` and GPU-validation runs passed; the final
GPU-validation run includes the transparency state fix. These regressions cover
frozen publication, zero steady-state
capture/filter, explicit rebake, setting changes, removal/recreation and both
existing capture modes. Local build/run logs and visually inspected PNG/PPM views
are under `Build/BistroAdditions` (ignored artifacts).

## Limits

No long-run SPH/crowd stress or performance tuning was performed. The paths have
no navigation or avoidance. Live SPH pause/time-scale behavior uses the existing
renderer delta-time path; no independent particle-buffer readback for it was added.
The MGPU sample supplied no separate soldier asset license; its original
attribution is preserved in `Assets/Models/MGPUSoldier/SOURCE.md`.

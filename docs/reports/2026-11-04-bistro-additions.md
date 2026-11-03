# Bistro scene additions — 2026-11-04

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

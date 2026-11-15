# Bistro particles in ordinary MetaRenderGraph

## Sources, time and settings

Ordinary Meta creates only `CreateBistroParticles` from BistroScene.h, alongside
the existing grass/wind. `BistroParticles::Update` runs before App::Update, so
controls/bursts precede RenderModule's emitter snapshot and GPU BeginFrame.
The opt-in `AdvancesParticlesWhileWorldPaused` pipeline capability lets the
collector bypass only world pause. Explicit emitter pause, world/emitter time
scales, substeps, reset revisions and cumulative BurstTotal remain unchanged.
Default App callers retain both previous pause checks. No physics, animation,
fluid/SPH, probes or other additions are created.

Startup explicitly selects Enabled/DrawEnabled/SimulationEnabled=true,
Render.Particles.Quality=2 (Mid), AsyncCompute=false, secondary adapter=-1,
Meta.Particles.Enabled=false (no Meta offload), Count=-1, CountScale=2,
EdgeSamples=1 and MinPixelSize=0. App source defaults request 500000 particles;
no saved startup Count/CountScale override was found in the inspected App configs.
UI quality presets exist separately and are not silently applied at startup.
Mid fills unset substeps with 2, without scaling explicit emitter capacities.
The existing SemiImplicitEuler simulation default is preserved.

| Effect | Requested pool budget |
|---|---:|
| EntranceFountain | 125000 |
| BlueFountain | 93750 |
| TerraceEmbers | 62500 |
| PlanterFireflies | 62500 |
| StreetFirebursts | 62500 |
| InteriorDust | 31250 |
| TableSteam | 31250 |
| SparkRain | 31250 |
| Total | 500000 |

Original seeds, lifetimes, shapes, atlas animation, emission/burst semantics,
blends, mesh Firefly/WaterDrop, soft particles, collision volumes/boxes and
placement are reused. No budget reduction or adjacent-quality reduction was made.
The entrance fountains are near (-170,45,-170) and (-70,45,65) cm; fireflies
occupy the original grass arch at BistroArch::SwarmBounds. Terrace embers,
interior dust/steam use the same `BistroBounds::Scene` setup/update as App.
This restores dust center (559,221,-348), half-extents (694,181,779), and steam
center (922,227,-293), half-extents (305,175,85.5), both at -28.5 degrees yaw.
The shared updater reserves billboard corners and enables reflecting confinement,
and retains the existing App.Bounds controls. Rain retains its authored position.

## Native execution and composition

`SceneParticles`, owned by ordinary SceneTransparency, uses the persistent primary
`resources.ParticleSimulation`. It never swaps the global simulation pointer.
Existing `GDX12ParticleSimulation` and `GDX12ParticlesPass` helpers record into
borrowed executor command lists under `GDX12RecordingStates`. Packets are DIRECT,
OwnerRecording, declare resource modes/states, restore boundary states and reject
private submission signals. No legacy graph Execute or per-frame Flush is added.
Existing helper waits remain allocation/reset/lifetime operations.

Groups retain their DepthCollision partition. Each group initializes/resets and
simulates its pools using settings/constants; the colliding group reads current
opaque depth after mesh and grass prepass, without glass or fluid merged depth.
The current grass interaction resource is declared as a read, giving its producer
an explicit hazard/dependency before firefly simulation. No character or synthetic
interaction source is added. Grass's accepted culling without bend is untouched.

Both groups append render inputs before the final global SortedAlpha sort:
group A clears/culls, group B appends/finalizes. The composition order is:

1. Particle GPU simulation and render preparation/culling/global sort.
2. Existing one All mesh WBOIT clear/draw (accumulation=0, revealage=1).
3. Existing DrawWeightedAndMotion appends to those same WBOIT targets.
4. One WBOIT resolve combines glass/weighted particles with HDR after SSR.
5. Existing Composite adds SortedAlpha/additive to that HDR, then display/PRESENT.

Accesses include persistent pools, per-frame emitter/camera constants, draw/sort
buffers, immutable mesh data, motion target, depth/interaction and imported atlas.
UPLOAD resources retain GENERIC_READ; helpers manage private transitions inside
declared DEFAULT read boundaries. Existing one-time Firefly mesh allocation is
reused. No CPU sorting, visibility readback or particle readback is introduced.

DrawEnabled and SimulationEnabled remain separate: drawing may stop while
simulation continues, and simulation may freeze while existing particles draw.
The shared pass's inactive/empty paths do not publish stale motion; its active
prepare/weighted paths reset counts/arguments and clear feature motion normally.
Extent changes use the existing Resize before BeginFrame after the pipeline's
drain. Destruction clears ParticleMotion and the borrowed interaction pointer.
SSR continues using opaque-only data; particles in reflections are out of scope.

## Verification and limits

Release x64 build passed. In the ordinary no-argument launch the user confirmed:
"Всё работает, частицы движутся". The saved current-run log has no ERROR/FATAL;
the operation list includes A/B simulation, global sort, mesh WBOIT clear/draw,
weighted append, one resolve and SortedAlpha/additive composition.
The user subsequently identified dust/steam layout mismatch: Meta had omitted
App's post-creation bounds setup. The shared bounds owner/updater is now reused;
the corrected Release x64 build passed (existing Jolt LNK4099 warnings only),
and the updated ordinary executable was launched for visual review. Independent
source review found no blockers, including App bounds parity and owner lifetime.
Visual confirmation of the corrected dust/steam volumes remains with the user.
No --smoke, added tests, runtime matrices, captures, profiling or long-run harness.
Existing ParticleTimeTests/particle simulation/render source was inspected for
context; reset/pause/zero-count and alternate flags are reviewed in source rather
than expanded runtime checks. No speedup or measured particle cost is claimed.

Native resolution, camera, scene/materials, AO/SSR, shadow and grass caches are
preserved. Animation/skinning and the next technique are not started.
Artifacts: `Build/Verification/meta-bistro-particles/`.
Executable: `D:/Project/PEPEngine2/Build/bin/x64/Release/App.MetaRenderGraph/App.MetaRenderGraph.exe`.
Pre-existing SceneManagerModule.cpp/BistroAdditionsTests.cpp changes and Temp/
are preserved and excluded from the task commit.

# App.MetaRenderGraph

Experimental executable using the Meta execution plan alongside production App.
The ordinary launch now loads the original Bistro interior and exterior with
static opaque materials: GPU culling, depth prepass, GBuffer, PBR, authored lights,
SkyAtmosphere/IBL, cascaded sun shadows, point/spot shadows, XeGTAO, FidelityFX SSSR, mesh WBOIT
transparency, Bistro grass/wind, GPU particles, animated people/foxes, SPH water, FidelityFX DoF, Motion Blur,
reconstruction, HDR display transform and Engine.UI screen composition. Alpha-tested cutouts use OpaquePass.
No reflection-probe, frame-generation
or world-space UI poster passes execute.
The world is paused; grass/wind, particles, characters and water advance through explicit time paths.
The camera remains free.

This adds Bistro Depth of Field to the [incremental migration](../../docs/plans/2026-11-15-bistro-incremental-migration.md).
The user checks the ordinary launch, measures CPU/GPU load and FPS, and chooses the
next stage. Final architecture/performance comparison waits until migration ends.

Controls match App: WASD movement, Q/E down/up, right mouse to look, Shift for
speed, mouse wheel to change speed. FPS and frame time appear in the title bar.
F1 shows/hides the existing Engine.UI Settings panel in Release too. Its search
and console-variable controls capture keyboard/mouse input before the free camera.
Startup uses App's 10 people and 5 foxes, original clips/routes/materials/scales,
skinning enabled at Mid (30 Hz), no pose-rate or actor preset override. Route jobs
complete before the render snapshot; physics remains paused. Skin palette uploads
and deformation precede depth/GBuffer/shadow consumers. Screen raster/composition
follows reconstruction and display once, at window resolution, without posters.
See the [animation/UI report](../../docs/reports/2026-11-15-meta-bistro-animation-ui.md).
The initial camera is at the Bistro entrance, as in App. The startup viewport is
1280x800; the render grid follows the effective reconstruction provider. The first frame's declared operations are
written to `Logs/opaque-operations.txt` beside the executable. The first reused
frame slot is recorded in `Logs/opaque-steady-operations.txt`; unchanged static
scene buffers have no upload/copy packets there. Camera and light-grid constants
use frame-local upload resources; light data, masks and shadow commands use VRAM.

The existing `--smoke` procedural diagnostic host remains separate. It is not the
short ordinary smoke required for acceptance and is not run for this migration.

Ordinary post order is opaque particle-water → mesh/weighted transparency with aerial → additive HDR → DoF → Motion Blur and complete motion
→ reconstruction/native → display → screen UI → PRESENT. Startup requests App's
XeSS/Default (Ultra Quality Plus) and Motion Blur Mid, shutter 0.5, max radius 40,
quality-selected samples (27). Settings retains all existing provider/preset,
blur and reconstruction restart controls. The Reconstruction window reports the
requested/effective provider, fallback reason and render/output sizes; F1 toggles
it with Settings. Native is passthrough without TAA; NIS is spatial; XeSS AA,
FSR NativeAA and DLSS DLAA use the existing providers. SDK availability determines
the explicit fallback. No provider matrix or performance claim is implied.
Native/NIS without blur skip motion preparation and particle motion; temporal SR
keeps complete motion even without gather. Context/grid changes drain before
resize and reset history. UI stays at window resolution, after postprocessing.
See the [Motion Blur/SR report](../../docs/reports/2026-11-15-meta-bistro-motion-blur-upscalers.md).

DoF uses App's FidelityFX pass and physical CoC model on the current render grid,
with RGBA16F HDR and matching reverse-Z composition depth (including opaque water).
Startup sets `Render.DOF.Enabled=true`, `.Quality=2` (Mid), `.FocusDistanceCm=500`,
`.FNumber=2.8`, `.SensorHeightMm=24`, `.CocLimit=0`, `.Rings=0`, `.RingMerge=true`.
Use F1 Settings to edit these existing CVars: focus distance selects the sharp plane
in centimetres; lower f-number widens blur; quality 0/1/2/3/4 is Off/Low/Mid/High/Epic.
Zero rings/CoC limit uses the quality preset (Mid: 5 rings, 0.05 limit).
`Render.DOF.FixedCocLimit` keeps the existing fixed-0.05 rule; an explicit positive
CoC limit takes precedence. No autofocus is added. Glass and translucent particles
retain App's limitation: they do not supply their own full DoF depth.
Disabled/Off or grids smaller than 16 in either dimension pass HDR straight through,
without a DoF packet, dispatch or copy. SDK context/targets persist; resize/SR grid
changes drain the owner before rebinding/resizing. Rings/limit/merge changes safely
retire the SDK context; focus/aperture/FOV update CoC without recreating it.
Enable/disable and CoC changes reset temporal SR history; unchanged focus parameters
do not reset it each frame. The SDK FP32 HDR accumulation fix is preserved.
See the [DoF report](../../docs/reports/2026-11-16-meta-bistro-dof.md).

The original stone basin is at (-1297,65.3,-425) cm, yaw 135 degrees, beside the
existing SparkRain emitter. Startup requests one basin and 100000 SPH particles,
HeightScale=2, ReferenceModel, original wave forcing/radius/filter, Fluid Mid,
primary GPU DIRECT without offload. Seed size follows App's grid rounding and is
logged on startup. The solver advances while world physics stays paused; component
simulation/render controls and time scales remain effective. Surface uses the SR
render grid and retains solver pools across screen resize. Water currently draws
opaque velocity-colored particles before mesh/weighted WBOIT; its depth and color
receive aerial once in the opaque composition. Particle collisions read opaque
depth; translucent particle raster retains App's depth rejection and ordering.
The former far/water-optics/near path is not active. See the
[water report](../../docs/reports/2026-11-16-meta-bistro-water.md).

Ordinary startup selects `Meta.Sky.Source=2` (SkyAtmosphere); `1` selects the
original SkyBox and `0` Off. Existing `Render.SkyAtmosphere.Enabled` and `.Quality`
remain effective. Mid uses transmittance 256×64, multiple scattering 32×32,
SkyView 256×128, aerial 32³ and a 256-face IBL cube. The original Sun entity's
rotation drives atmosphere direction, the visible disk and shadow lighting.
`Meta.Sky.SunCycle.Enabled=true` rotates this same Sun through sunrise → zenith →
sunset in `Meta.Sky.SunCycle.PeriodSeconds=120`, then returns directly to sunrise,
without a night segment. Disable it for manual light rotation; world physics stays paused.
Atmosphere component settings retain control of the medium and solar illuminance.
Ordinary Meta initializes the solar angular radius to 0.0116875 radians (2.5×
the shared default), increasing the visible disk without changing illuminance.
Transmittance/MS and fixed-position IBL are cached by their inputs; camera motion
updates SkyView/aerial without refiltering IBL or resetting reflection history.
Sun motion refreshes IBL as lighting changes, while preserving temporal history;
the sunset-to-sunrise jump resets history once per cycle.
Opaque/SSR stay unfogged until WBOIT resolves aerial once; glass applies its own
aerial, and screen UI remains after postprocessing. Probes and global clouds stay
disabled. See the [atmosphere report](../../docs/reports/2026-11-16-meta-bistro-sky-atmosphere.md).

## Build and ordinary launch

From a Visual Studio developer command prompt at the repository root:

```bat
msbuild Apps\App.MetaRenderGraph\App.MetaRenderGraph.vcxproj /t:Build /p:Configuration=Release /p:Platform=x64 /m:1 /nr:false
Build\bin\x64\Release\App.MetaRenderGraph\App.MetaRenderGraph.exe
```

Debug x64 is the other project configuration. Build through the repository's
existing toolset and SDK settings. Production App has separate output/config
paths and retains its existing executor.

An ordinary launch has no arguments. `--smoke` selects the old expanded
diagnostic harness, including readbacks/comparisons and resize scenarios; it is
not the short ordinary smoke required by the current acceptance rules.

## Configuration

The settings below also describe the retained procedural implementation. The
ordinary opaque stage does not enable extra techniques through these switches;
secondary-adapter opt-ins are rejected. Ordinary sun shadows use App's default
2048 texels per cascade (4096x4096 depth atlas); local shadow resolution defaults
to 256 texels per face (`Render.Shadows.Local.Resolution`, `0` disables local shadows).
`--smoke` restores its previous small diagnostic shadow sizes.

Ordinary Bistro runs the existing slot-local `SceneAO`/`GDX12AmbientOcclusionPass`
after GBuffer and before deferred lighting, using depth and GBuffer normals.
Startup selects XeGTAO, Mid, Full, BeforeLighting: radius 50 cm, intensity 1,
two denoise passes inherited from Mid, two slices and two steps per half slice.
`Render.AO.Enabled=false` writes neutral visibility 1 without AO/denoise dispatches.
The output is read by deferred lighting at `t14`, affecting diffuse IBL as in App.
Early AO remains disabled in ordinary Bistro; other excluded techniques stay off.
See the [AO stage report](../../docs/reports/2026-11-15-meta-bistro-ao.md).

Mesh transparency uses one existing WBOIT `All` draw and one resolve after
deferred lighting, with read-only opaque depth. Each frame slot owns accumulation,
revealage and composed HDR; accumulation/revealage clear to 0/1 even when no
transparent draw survives GPU culling. The unbatched indirect stream uses source
instance IDs. Forward lighting uses the same published local-shadow slices and
face masks as deferred lighting, plus sun shadows and static IBL, without opaque
screen AO, local probes or aerial perspective. App's default shader path is unchanged.
Look at the entrance door/window panes, menu glass, and wine bottles on outdoor
tables or behind the interior bar. After visual review, drinking-glass opacity
is 0.50 with shininess 100; other glass materials retain their authored settings.
Blended bounds participate in local-shadow receiver selection; they do not cast
opaque shadows. See the [transparency report](../../docs/reports/2026-11-15-meta-bistro-transparency.md).

FidelityFX SSSR and its denoiser run after deferred lighting and before mesh
transparency. Startup selects `Render.SSR.Enabled=true`, `Render.SSR.Quality=2`
(Mid), with inherited App defaults. Misses use static IBL, without reflection
probes. Resolve replaces the original specular IBL contribution once. Disabling
SSR bypasses its packets and sends opaque HDR directly to transparency.
The SDK history persists across frame-slot rotation; camera cuts, resize,
settings/environment changes and re-enable reset it. The three frames in flight
fit the bundled SDK's four-frame limit.
Look at the metal bar/counter and other glossy opaque surfaces inside Bistro.
Only opaque surfaces present in screen-space depth/color can appear in these
reflections; glass retains static IBL and is not captured by SSR.
See the [SSSR stage report](../../docs/reports/2026-11-15-meta-bistro-ssr.md).

Grass reuses `BistroGrass::Scene` and the App GPU wind/expansion/render kernels.
The initial mode is one Ribbon patch with Analytic wind, both at Mid quality,
at the original building_02 arch/tunnel (centre 1399.6, 23.3, 1369 cm, yaw 69 degrees).
Grass writes depth/GBuffer/velocity and participates in sun/local shadows.
Wind advances while the rest of the world is paused, retaining world/field time
scales and the existing bounded wind step/reset rules. No interaction sources
are created; the interaction field is initialized to its neutral state.
Ordinary grass uses DIRECT and omits diagnostic probe targets/readbacks.
Local deformation refreshes affected maps while retaining cached static mesh
lists. See the [grass/wind report](../../docs/reports/2026-11-15-meta-bistro-grass-wind.md).

Particles reuse the eight existing `BistroParticles` effects and the primary GPU
simulation/render passes. Startup requests 500000 particles (Count=-1,
CountScale=2), Quality=2, with simulation and drawing enabled, async/offload off.
Emitter pause and time scales remain effective while world pause is bypassed only
for this ordinary path. Colliding emitters read current mesh/grass opaque depth;
fireflies read the current grass interaction field. Weighted particles append
after the glass draw, before the single WBOIT resolve; global SortedAlpha/additive
composition follows that resolve. Particle feature motion is enabled when blur or
temporal reconstruction consumes it; history remains available when it is disabled.
Raster uses slot-local read-only opaque depth
with reversed-Z early depth testing; soft fading is preserved. Visible particles
reserve draw slots per wave (with a non-WaveOps fallback). Firefly vertices live
in DEFAULT memory after one recorded upload.
Look for the orange/blue fountains at the entrance, terrace embers, and fireflies
in the original grass arch; dust/steam use App's `BistroBounds::Scene` layout
volumes and confinement, while SparkRain retains its authored placement.
See the [particles report](../../docs/reports/2026-11-15-meta-bistro-particles.md).

The ordinary sun path uses App's `ShadowPass.hlsl` and `CSM.hlsli`: one GPU
culling dispatch for the active cascades, then one ExecuteIndirect draw per cascade directly
into the lighting atlas. There are no intermediate depth maps, packing kernels,
probe shadow maps or readbacks. Scene buffers stay resident; caster bounds are
reduced only when scene data changes. Atlas and visibility buffers belong to
the frame slot, and lighting declares the atlas read in the Meta graph.

Local shadows preserve authored `CastShadows` and update-mode settings. CPU jobs
clip receiver bounds to the camera frustum, then select light faces intersecting
those visible bounds and the light range. This is conservative bounds culling,
without depth/occlusion readback. Casters for selected faces are still gathered
from the whole scene BVH, including objects outside the camera. The selection
includes the normal-bias offset used by the lighting shader.

Each frame slot caches per-face command lists and depth maps with stable slice
assignments. Camera movement selects faces without invalidating existing maps;
newly needed faces render before lighting. Mesh/instance/material changes,
texture-resource rebinding, light generation/projection/revision changes and
resolution changes invalidate the relevant cache. Material `DirtyFlag` also
invalidates maps when GPU constants stay equal, allowing alpha-mask refreshes.
In-place texture writers must mark the material dirty or explicitly invalidate
the light. `EveryFrame` lights continue to refresh selected faces every frame.

Scene inputs are updated from changed buffer-element journals, after ECS upload
jobs finish. Reverse instance/transform/material references resolve only affected
objects; camera transforms have no mesh references. Existing BVH leaves and their
ancestors are refitted, and intersecting light faces are invalidated using both
old and new caster bounds. Topology changes rebuild the BVH. Receiver selection
traverses the BVH only when the camera, scene or light changes, without a separate
whole-scene clipping job. Frame-local scene uploads copy changed ranges rather
than taking a full CPU snapshot when only a camera transform changed.

Only changed faces upload commands, clear and draw. Fully opaque casters use a
depth-only PSO without a pixel shader; masked casters keep App's alpha-test PSO.
Depth bias and filtering are unchanged. Map validity is published after frame
submission. The array supports up to 2048 resident slices per slot; if residency
fills, camera-irrelevant allocations are reclaimed and required maps refreshed.

The ordinary light grid is built by `LightGrid.hlsl` on the GPU. Its source SRV
and mask UAV stay in VRAM; source uploads occur only when their revision or
resource identity changes. One compute pass writes complete mask words without
atomics or a separate clear. Neighboring cells in one depth slice share a light
index across the wave; wave bounds reject irrelevant lights. No mask data is
built on or read back to the CPU. Small frame constants still use upload buffers.
The retained diagnostic mode and production App keep their existing CPU builder.

Sun CSM settings use the URP-style count, maximum distance and normalized split
boundaries. These CVars apply at the frame boundary in App and MetaRenderGraph:

| CVar under `Render.Shadows.Sun.` | Default | Meaning |
|---|---|---|
| `Resolution` | `2048` | Texels **per cascade**, retaining the existing meaning; `0` disables sun shadows. Allowed sizes: 512, 1024, 2048, 4096. |
| `CascadeCount` | `4` | Active cascades, 1 through 4. |
| `MaxDistance` | `6000` | Maximum distance in scene units (Bistro: centimetres), capped by camera far plane. At or below the camera near plane, sun shadowing is omitted. |
| `AutomaticSplits` | `true` | Preserve the existing logarithmic/linear distribution. Set `false` to use the split controls below. |
| `CascadeSplit1` | `0.067` | First normalized boundary; used with 2–4 cascades. |
| `CascadeSplit2` | `0.2` | Second normalized boundary; used with 3–4 cascades. |
| `CascadeSplit3` | `0.467` | Third normalized boundary; used with 4 cascades. |

Manual boundaries divide the range from camera near plane to the effective
maximum shadow distance. Active boundaries must increase strictly within `(0,1)`;
invalid scalar values are rejected by the console. An unordered active tuple
emits a warning and retains the last valid tuple for that cascade count, so
editing several controls cannot crash rendering. Inactive boundaries are ignored.
The final active cascade fades to unshadowed lighting. The atlas remains 2x2;
inactive cascades are not culled or drawn. URP's Shadow Resolution instead means
the maximum **whole atlas** size; this project intentionally preserves its
existing per-cascade resolution setting.


The build copies `Configs/Startup.ini` beside the executable. On every launch,
`PrepareStartup` copies that file over the executable's `Config/Engine.ini`
before reading settings. Edit the output `Configs/Startup.ini` for a local run;
edit this project's source copy when changing persistent defaults. Settings
saved only in the generated `Config/Engine.ini` are replaced on the next start.

For one card with all work placed on DIRECT, keep the other Startup.ini settings
and set:

```ini
Meta.Async.Queues = false
Meta.CrossAdapter.Enabled = false
Meta.Water.Enabled = false
Meta.Particles.Enabled = false
Render.PrimaryAdapter = 0
Render.Particles.SecondaryAdapter = -1
Render.Fluid.SecondaryAdapter = -1
Render.Upscaler.Provider = 0
Render.FrameGeneration.Provider = 0
```

| Control | Meaning |
|---|---|
| `Meta.Async.Queues` | `false`: single-DIRECT placement. `true`: selected local COMPUTE/COPY packets; currently the registered default. |
| `Render.Upscaler.Provider` | `0`: Native. `2`: FSR. These are the current reconstruction scope. |
| `Render.FrameGeneration.Provider` | Keep `0`; FG and other reconstruction providers are deferred. |
| `Meta.Water.Enabled` | Opt in to the Meta secondary water solver; default `false`. |
| `Meta.Water.SecondaryAdapter` | Explicit secondary DXGI hardware adapter index, distinct from the actual primary. Required when water transfer is enabled. |
| `Meta.Water.Selection` | `0`: StrictPublication. `1`: LatestCompleted; default `1`. |
| `Meta.Particles.Enabled` | Opt in to secondary simulation for every `SharedSimulation=true` and `DepthCollision=false` emitter; default `false`. Depth-collision emitters remain primary. |
| `Meta.Particles.SecondaryAdapter` | Explicit secondary index for particles-only mode. With Meta water enabled, `-1` shares the water adapter; an explicitly different adapter is rejected. |
| `Meta.Particles.Selection` | `0`: StrictPublication. `1`: LatestCompleted; default `1`. Independent of water selection. |
| `Meta.CrossAdapter.Enabled` | Separate U9 deterministic buffer-transport fixture; default `false`. It excludes water/particle offload and diagnostic verification mode. |
| `Meta.CrossAdapter.SecondaryAdapter`, `.Selection`, `.Seed` | Explicit adapter, publication selection and seed for that buffer fixture. |

Legacy `Render.Particles.SecondaryAdapter` and `Render.Fluid.SecondaryAdapter`
remain `-1` so their private offload runtimes do not run beside the Meta executor.
The current host requires primary index `0`. The selected secondary must be a
different hardware device; there is no automatic single-card fallback for MGPU.

For particles-only MGPU, enable `Meta.Particles.Enabled` and set its secondary
index explicitly. For water and particles together, enable both, set
`Meta.Water.SecondaryAdapter`, and leave `Meta.Particles.SecondaryAdapter=-1` to
share that device. Keep `Meta.CrossAdapter.Enabled=false`. `Meta.Async.Queues`
continues to control local primary placement, not whether secondary offload runs.

Water and particles share one `SimulationDomain` device/submission owner, with
independent producers. Particle publication has one combined payload export and
one import; the shared interaction-field input is transferred separately.
Imported and local particles use the common transparency sort/resolve.

The retained procedural diagnostic scene creates water and four continuous emitter
groups near the camera: local depth-independent, local depth-collision, eligible
secondary, and marked depth-collision fallback. With offload disabled all remain
local; with local async enabled the depth-collision groups use COMPUTE. This is
scene content, not a new test harness.

## Current result and limits

- Local shadows now add receiver/face selection, separate opaque depth-only
  rendering and temporal map reuse; see the
  [cache implementation report](../../docs/reports/2026-11-15-meta-local-shadow-cache.md).
- The initial CPU jobs for local shadows and their shader variants built in Release.
  Runtime appearance and performance await the user's manual check; see the
  [local-shadow report](../../docs/reports/2026-11-15-meta-local-shadow-jobs.md).
- Sun shadows built in Release and displayed in the ordinary Bistro launch.
  Further runtime checks were stopped at the user's request; user acceptance
  is pending. See the [sun-shadow report](../../docs/reports/2026-11-15-meta-sun-shadows.md).
- The Bistro opaque stage passed Release build and ordinary visual smoke. The
  final short entrance-view measurement was 356 FPS and 97% GPU utilization;
  the user accepted the result. See the [stage report](../../docs/reports/2026-11-15-meta-opaque-bistro.md).
- The earlier local DIRECT launch displayed procedural planes and grass on an
  RTX 3060 Laptop GPU and exited normally. See the
  [local smoke report](../../docs/reports/meta-render-graph/local-direct-smoke-2026-11-14.md).
- Subsequent integrated runtime fixes, removal of ordinary-frame diagnostic
  copies and U11 have source review only. That earlier run does not validate the
  new water/particle scene or MGPU.
- The ordinary plan omits diagnostic readback copies; some diagnostic buffers
  remain allocated. No performance improvement is claimed.
- Current handoff status is in the [U12 report](../../docs/reports/meta-render-graph/12-acceptance.md)
  and [technique inventory](../../docs/reports/meta-render-graph/technique-inventory.md).

Acceptance is a short ordinary launch: starts, GPU performs work, a result is
displayed, no obvious error. Numeric precision, mode matrices, edge-case suites,
profiling and repeated runs require a separate user request. In source-only mode,
builds and application runs are not performed.

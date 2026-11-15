# App.MetaRenderGraph

Experimental executable using the Meta execution plan alongside production App.
The ordinary launch now loads the original Bistro interior and exterior with
static opaque materials: GPU culling, depth prepass, GBuffer, PBR, authored lights,
static SkyBox/IBL, cascaded sun shadows, point/spot shadows, XeGTAO, mesh WBOIT
transparency and HDR display transform. Alpha-tested cutouts use OpaquePass.
No SSR, reflection-probe, grass, water, particle, reconstruction, post-effect
or screen-UI passes execute.
The world is paused to exclude simulation; the camera remains free.

This adds mesh transparency to the [incremental migration](../../docs/plans/2026-11-15-bistro-incremental-migration.md).
The user checks the ordinary launch, measures CPU/GPU load and FPS, and chooses the
next stage. Final architecture/performance comparison waits until migration ends.

Controls match App: WASD movement, Q/E down/up, right mouse to look, Shift for
speed, mouse wheel to change speed. FPS and frame time appear in the title bar.
The initial camera is at the Bistro entrance, as in App. The startup viewport is
1280x800; render resolution is native. The first frame's declared operations are
written to `Logs/opaque-operations.txt` beside the executable. The first reused
frame slot is recorded in `Logs/opaque-steady-operations.txt`; unchanged static
scene buffers have no upload/copy packets there. Camera and light-grid constants
use frame-local upload resources; light data, masks and shadow commands use VRAM.

The existing `--smoke` procedural diagnostic host remains separate. It is not the
short ordinary smoke required for acceptance and is not run for this migration.

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

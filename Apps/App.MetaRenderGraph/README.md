# App.MetaRenderGraph

Experimental executable using the Meta execution plan alongside production App.
The ordinary launch now loads the original Bistro interior and exterior with
static opaque materials: GPU culling, depth prepass, GBuffer, PBR, authored lights,
static SkyBox/IBL, cascaded sun shadows and HDR display transform. Alpha-tested
cutouts use OpaquePass; blended materials are omitted. No local-shadow, AO, SSR, reflection-probe, grass, water,
particle, transparency, reconstruction, post-effect or screen-UI passes execute.
The world is paused to exclude simulation; the camera remains free.

This adds the sun-shadow stage of the [incremental migration](../../docs/plans/2026-11-15-bistro-incremental-migration.md).
After the ordinary smoke, the user measures CPU/GPU load and FPS and chooses the
next stage. Final architecture/performance comparison waits until migration ends.

Controls match App: WASD movement, Q/E down/up, right mouse to look, Shift for
speed, mouse wheel to change speed. FPS and frame time appear in the title bar.
The initial camera is at the Bistro entrance, as in App. The startup viewport is
1280x800; render resolution is native. The first frame's declared operations are
written to `Logs/opaque-operations.txt` beside the executable. The first reused
frame slot is recorded in `Logs/opaque-steady-operations.txt`; unchanged static
scene buffers have no upload/copy packets there. Camera constants and light-grid
inputs bind their existing frame-local upload resources directly.

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
2048 texels per cascade (4096x4096 depth atlas); local shadow resolution stays
zero. `--smoke` restores its previous small diagnostic shadow sizes.

The ordinary sun path uses App's `ShadowPass.hlsl` and `CSM.hlsli`: one GPU
culling dispatch for four cascades, then four ExecuteIndirect draws directly
into the lighting atlas. There are no intermediate depth maps, packing kernels,
probe shadow maps or readbacks. Scene buffers stay resident; caster bounds are
reduced only when scene data changes. Atlas and visibility buffers belong to
the frame slot, and lighting declares the atlas read in the Meta graph.


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

# Bistro App

`App.vcxproj` is the only application target in `PEPEngine2.sln`. It runs the
Bistro renderer. App.Base, Engine.Core,
Engine.RendererDX12, Engine.UI and External.Libraries remain shared libraries.

Build `Apps/App/App.vcxproj` with Visual Studio 2026/v145, `Release|x64`, then run
`Build/bin/x64/Release/App/App.exe` without arguments. Direct3D 12 and Shader
Model 6.0 or newer are required. DXC follows the adapter up to SM6.8.

## Rendering

`src/BistroApplication.cpp` owns scene setup. `src/Rendering/BistroRenderPipeline`
connects the sole packet graph in `src/Graph` with the techniques in
`src/Techniques`. Engine.RendererDX12 supplies resource management, shader kernels
and SDK integrations. The old default graph, duplicate pass recording and
procedural diagnostic renderer have been removed.

The scene retains the original interior/exterior, point and sun shadows,
reflection probes, grass, the SPH particle basin in `src/Techniques/Water`, particles, walking NPCs/foxes, physics,
offscreen UI canvases and screen UI. The cloud picture retains its secondary
adapter transport. Retired experimental particle/water transports and the old
Editor/Benchmark application targets are not part of this renderer.

`App.Async.Queues=true` lets the graph place local work on COMPUTE/COPY;
`false` uses DIRECT. Scheduling is controlled by the graph, with no separate
legacy per-technique async switches.

### Early camera DepthNormals

DepthNormals is enabled by default. Set `App.DepthNormalsPrepass = false`
in Startup.ini or the CVar editor to use the depth-only path. The packet
`scene DepthNormals ready` publishes depth and a separate world-space
normal/roughness target before the G-buffer. Opaque particles share one set of
culled draws across both passes. Their depth/normal draw follows actual readers
of the original scene depth, including particle depth collision. World canvases
remain late overlays with read-only depth.

Use `--read-texture=Scene.DepthNormals.Normals` or
`--read-texture=Scene.DepthNormals.Depth` to read at the early graph boundary.

### Two-adapter synchronization

In F1, each technique can select its GPU by name. `App.CloudPicture.Adapter`,
`App.AO.Adapter`, `App.Particles.Adapter` and `App.Water.Adapter` use absolute
indices in the same hardware list as the startup-only `Render.PrimaryAdapter`.
The default `-1` and the current primary index both run locally. Techniques can
share a remote GPU or use different ones. Changing a selection takes effect
during the run. In the table, GPU0 means the primary and GPU1 means the selected
remote GPU for that technique.

Use `App.CloudPicture.CrossSync`, `App.AO.CrossSync`, `App.Particles.CrossSync`
or `App.Water.CrossSync` to choose how GPU0 consumes GPU1 publications. N is
the current frame's order in the publication history. Fence waits run on the
GPU0 import queue. A wait for an already completed export incurs no GPU stall.

| Mode | Who starts GPU1 work | Version used by GPU0 | Wait and first frame | Available techniques |
| --- | --- | --- | --- | --- |
| `0` | Bistro orders N | Exactly N−1 | Wait for N−1 if pending. First uses N, so the first pairs are 1/1, 2/1, 3/2. | Cloud Picture **(default)**, AO, particles, water **(default)** |
| `1` | Bistro orders N | Exactly N | Wait for N if pending, including the first frame. | Cloud Picture, AO **(default)**, particles **(default)**, water |
| `2` | GPU1 advances independently | Newest completed publication | Cloud Picture and water can wait for the first issued export token, then wait on that fence at import. Particles start empty until a snapshot completes. After bootstrap each technique uses completed exports. | Cloud Picture, particles, water |
| `3` | Bistro orders N | Newest completed publication | Starts empty until the first export completes. No wait for pending results at import. | Particles |

For AO, mode `0` reprojects the previous result into the current frame.

### Remote ambient occlusion

Set `App.AO.Adapter` to a secondary DXGI index from the same list as
`Render.PrimaryAdapter`. The default `-1` runs AO locally. An unavailable
adapter, an unsupported grid, or a missing required prepass falls back to local AO.

| `Render.AO.Implementation` | Remote grids (`Render.AO.Resolution`) | GPU0 → GPU1 | Filter |
| --- | --- | --- | --- |
| `0` SSAO | Full (`0`), Half (`1`) | Early depth and material normals | Bilateral on GPU0 or GPU1 |
| `1` HBAO | Half (`1`) | Half-depth | Bilateral on GPU0 or GPU1 |
| `2` XeGTAO | Full (`0`) | Early depth and material normals | XeGTAO denoise on GPU0 or GPU1 |
| `3` CACAO | Full (`0`), Half (`1`) | Early depth and material normals | CACAO SDK on GPU1 |

SSAO, XeGTAO and CACAO require `App.DepthNormalsPrepass = true`. GPU0 prepares
and exports their inputs at the DepthNormals boundary, before the G-buffer.
Half-resolution preparation preserves the representative pixel used by SSAO
and by the GPU0 depth/normal-aware upsample. CACAO retains its SDK settings
and the existing padding for small grids.

All four paths support `App.AO.CrossSync = 1` (current frame, default) and
`0` (exactly the previous frame). The first frame and the first after a history
reset import their own output. GPU0 reprojects delayed AO using G-buffer motion,
both frames' jitter, and the source depth. Invalid pixels use visibility 1.
Resolve and any upsample run on GPU0.

- SSAO and HBAO export one visibility texture. With `App.AO.BlurOnSecondary = 0`
  (default), GPU0 reprojects before bilateral blur and uses current depth and
  material normals. With `1`, GPU1 filters the source grid before export. SSAO
  uses the exported normals and HBAO reconstructs normals from half-depth.
- XeGTAO keeps the depth pyramid on GPU1. With `BlurOnSecondary = 0`, GPU1
  returns raw R8_UINT terms and R8_UNORM edges when denoise is enabled. GPU0
  denoises on the source grid with that frame's parameters before reprojection.
  With `1`, GPU1 denoises and returns only terms. Zero passes export only raw
  terms and use the raw-terms scale once.
- CACAO returns the SDK's final visibility. Its configured blur passes run in
  the SDK context on GPU1. `App.AO.BlurOnSecondary` has no effect on CACAO.

Use `--read-texture=Scene.AO.Reprojected` to capture the final `Scene.AO` once
a delayed frame has consumed history.

### Remote SPH water

`App.Water.Adapter = -1` (default) keeps the local SPH path and
`App.Async.Water` queue selection. Set it to a secondary DXGI index to run SPH
on GPU1. An unavailable adapter falls back to local SPH.

`App.Water.CrossSync = 0` defaults to exact N−1 when a secondary adapter is
selected. Mode `1` imports exact N. Mode `2` advances SPH on GPU1 with a
monotonic clock scaled by game time and the instance's time scale. GPU0
imports the newest completed display snapshot each visible frame, including
repeats. The first import can use a seed or frozen state while paused. The
producer sleeps on control or fence events when it cannot advance. Changing
mode starts a new publication history and keeps the GPU1 pool and wave phase.
GPU1 sends position and speed.
GPU0 draws with its current camera, HDR and depth. Pausing or hiding water
keeps its pool, and changing GPU owners resets it from the authored seed.

### Remote particles

Set `App.Particles.Adapter` to a secondary DXGI index from the same list as
`Render.PrimaryAdapter`. The default `-1` runs simulation on GPU0. Marked
transparent emitters with frame-only inputs can use GPU1. Bistro enables this
for Terrace Embers, Interior Dust and Table Steam. These emitters share one
publication. Planter Fireflies use a separate publication and import the
agent interaction field on GPU1. GPU0 imports their result before the opaque
cull and draws both DepthNormals and G-buffer from that choice. GPU0 keeps
frustum culling, sorting, soft-particle depth and all drawing.
Entrance Fountain, Blue Fountain, Street Firebursts and Spark Rain share a
separate publication. GPU1 receives the scene prepass depth and matching camera
constants at the collision point. GPU0 imports their result before transparent
cull. Depth collision and analytic colliders run in the existing particle solver.
An unavailable adapter falls back to local simulation.
Changing the owner resets the pool and preserves the already consumed burst
total for the same emitter revision.

`App.Particles.CrossSync = 1` (default) imports the exact step ordered by
the current frame. With `0`, the same frame orders step N and imports N−1.
The first frame of each history shows its own step. Changing between these
policies starts fresh publication history and keeps the live GPU1 pool.
With `3`, Bistro orders one step per frame and displays the newest completed
snapshot. The source stays empty until a snapshot completes. Repeated frames
reuse the GPU0 copy. Busy output slots can skip publication while the solver
continues advancing. The bounded input upload ring may wait for prior use
to retire. This is separate from the import, which selects completed exports.
With `2`, GPU1 advances independently using a monotonic clock scaled by
game time. Pause freezes the solver and resume excludes paused time. The
producer sleeps on fences or control events when it cannot publish. GPU0
uses the same completed-snapshot selection and import cache as mode `3`.
Frame-driven modes use the input of the ordered frame. In mode `2`, input-dependent
producers start when the first external input completes and use the newest
complete input at each step.
Repeats and gaps reset particle position, age and rotation history when
there is no immediately preceding displayed step. Camera motion remains.
`App.Particles.LogSnapshots = true` enables a short readback
of remote draw elements that passed the GPU0 particle cull.

### Read back a scene texture by name

Run `App.exe --read-texture=Scene.AO` for a one-shot readback without opening the
inspector. Use an exact name from the scene texture catalog, for example
`Scene.Depth` or `GBuffer.NormalRoughness`. Future techniques can register a
stable name with `SceneTextureInspector::Name`. The source must be a
single-sample texture bound to the GPU0 graph.

After the captured frame slot retires, Bistro writes tightly packed bytes for
subresource 0 to `Logs/SceneTexture.bin` (replacing the previous dump).
`Logs/Engine.log` provides the DXGI footprint format, width, height, depth,
row bytes, and row count. For R32_FLOAT it also reports the finite range,
non-finite count, and visibility counts for values below 0.999 and outside [0,1].

## Startup and controls

`Configs/Startup.ini` is copied beside the executable by the build. At each start
it replaces `Config/Engine.ini`. If a configured adapter index is missing,
Bistro writes primary index `0` and sets all four technique indices to `-1`
in both files. The log identifies the missing indices, the GPU running each
technique and the source of each technique's first remote import.
Edit the source Startup.ini to make startup settings persistent across builds.
Bistro starts at Medium quality with Balanced TAAU (provider 5, quality 3).
With provider 5, quality 0 or 1 selects native TAA. Presets
`2` (Quality), `3` (Balanced), and `4` (Performance) enable TAAU at roughly
67%, 59%, and 50% of output width and height, respectively.
Frame generation defaults to Off (`Render.FrameGeneration.Provider=0`); FSR FG
and XeFG remain available through the same presentation integration.

Use WASD/QE to move, Shift to accelerate and right mouse drag to look. F1 toggles the
settings UI. Scene bounds can be enabled with `App.Bounds.Show`.

Default scene populations are four reflection probes, one grass patch, one water
basin, ten NPCs and five foxes. Character counts apply at startup. The default
physics count scale is 1, preserving the original props without tableware copies.

Independent allocator, physics, content and GPU-kernel checks remain standalone.
Fixtures that instantiated the deleted renderer or diagnostic graph were retired;
MotionBlur/DoF kernel fixtures use the same preparation/recording API as App.
Historical reports under `docs/` describe their original binaries and paths.

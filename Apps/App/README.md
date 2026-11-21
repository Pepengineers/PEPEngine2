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

### Two-adapter ambient occlusion

Set `App.AO.Adapter` to a secondary DXGI index from the same list as
`Render.PrimaryAdapter`. The default `-1` runs AO locally. When Cloud Picture
also uses a secondary GPU, both techniques must use that adapter. An unavailable
or conflicting adapter, an unsupported grid, or a missing required prepass falls
back to local AO.

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

### Two-adapter particles

Set `App.Particles.Adapter` to a secondary DXGI index from the same list as
`Render.PrimaryAdapter`. The default `-1` runs simulation on GPU0. Marked
transparent emitters with frame-only inputs can use GPU1. Bistro enables this
for Terrace Embers. GPU0 keeps frustum culling, sorting, and all drawing.
An unavailable or conflicting adapter falls back to local simulation. Cloud
Picture takes priority when selecting the shared secondary adapter, then AO.
Changing the owner resets the pool and preserves the already consumed burst
total for the same emitter revision.

`App.Particles.CrossSync = 1` (default) imports the exact step ordered by
the current frame. `App.Particles.LogSnapshots = true` enables a short readback
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
it replaces `Config/Engine.ini`.
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

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

### Two-adapter HBAO

Set `Render.AO.Implementation = 1`, `Render.AO.Resolution = 1`, and `App.AO.Adapter`
to the secondary GPU index from the same hardware list as `Render.PrimaryAdapter`.
`App.AO.Adapter = -1` selects local AO, which is the default. When Cloud Picture
also uses a secondary GPU, both techniques share that adapter. An unavailable or
conflicting AO adapter falls back to local HBAO.

`App.AO.CrossSync = 1` (default) imports the current frame's visibility.
With `0`, GPU0 imports exactly the preceding frame and reprojects it using
G-buffer motion, jitter and a history depth check. Invalid history pixels use
visibility 1. The first frame and the first frame after invalidation import
their own output as a new history baseline.

`App.AO.BlurOnSecondary = 0` (default) runs the configured bilateral passes on
GPU0 after import and any reprojection, using current half-depth and G-buffer
normals. With `1`, GPU1 filters in the source grid before export using normals
reconstructed from half-depth. Both modes transfer only half-depth and one
visibility texture. Zero denoise passes skips blur on either GPU. Resolve and
upsampling always run on GPU0. To capture the final AO only after reprojection,
use `--read-texture=Scene.AO.Reprojected`. Launch with `--debug-ui` and select `Scene.AO` in the
scene texture inspector to view the result.

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

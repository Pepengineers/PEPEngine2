# Bistro App

`App.vcxproj` is the only application target in `PEPEngine2.sln`. It runs the
Bistro renderer formerly hosted by App.MetaRenderGraph. App.Base, Engine.Core,
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
reflection probes, grass, fluid basin, particles, walking NPCs/foxes, physics,
offscreen UI canvases and screen UI. The cloud picture retains its secondary
adapter transport. Retired experimental particle/water transports and the old
Editor/Benchmark application targets are not part of this renderer.

`Meta.Async.Queues=true` lets the graph place local work on COMPUTE/COPY;
`false` uses DIRECT. Scheduling is controlled by the graph, with no separate
legacy per-technique async switches.

## Startup and controls

`Configs/Startup.ini` is copied beside the executable by the build. At each start
it replaces `Config/Engine.ini`, matching the former ordinary Meta application.
Edit the source Startup.ini to make startup settings persistent across builds.
Native TAA (`Render.Upscaler.Provider=5`, quality 0) is the default.
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

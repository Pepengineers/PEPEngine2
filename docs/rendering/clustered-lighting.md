# CPU frustum grid and deferred light scalarization

The deferred pass uses a regular view-frustum grid. CPU light assignment does not
read the depth buffer. G-buffer depth is used only to reconstruct a shaded point
and choose its grid cell. Grid boundaries follow the jittered projection used by
the G-buffer; camera motion changes the transformed light bounds, not the definition
of the view-space grid.

`GDX12LightGridBuildSystem` transforms light spheres once, determines their Z range,
clips each sphere to a depth slab, and projects a conservative XY bound into that
slab. It writes the light's global ID into every touched cell. Spotlights use their
range sphere; their exact cone is evaluated by the existing PBR function. The last
slice covers the remaining visible range, including an infinite reverse-Z far plane.
There is no 512-light tile-list cap or overflow fallback.

The system is a separate `ParallelJob` on the application's shared JobSystem. Jobs
own disjoint Z slabs, so filling and clearing masks need no atomics. The system
finishes before GPU upload. `Jobs.ThreadCount` chooses the pool size; batch size is
bounded by `Jobs.BatchSize` and the slices per worker so the default batch does not
serialize a small grid. Standalone clients may omit the pool and build serially.

Each buffered frame owns an upload buffer, a GPU mask buffer and grid constants.
Only the current frame slot is changed after its fence has completed. The pass
records the mask copy and its internal resource barriers before shading. As with
other frame constants, callers must not overwrite a slot still used by the GPU.

In deferred, each lane fetches its cell's mask. `WaveActiveBitOr` merges masks,
`firstbitlow` selects the next source, and `WaveReadLaneFirst` explicitly makes its
index uniform. Only lanes whose own mask contains that source evaluate it. Thus a
source present in several cells is visited once per wave. BRDF, attenuation and
PCF remain per-pixel operations. Hardware without SM6 WaveOps uses the same grid
with a per-lane bit loop. The GPU pass has no groupshared list or group barriers.
Sun/IBL/probe/AO/PCF contracts and forward OIT/grass/probe-capture lighting remain
as before.

## Startup settings

All grid tuning uses statically registered ReadOnly CVars. Set these in the
executable's `Config/Engine.ini` and restart; an existing executable INI is not
replaced by the repository template during a normal build.

| CVar | Default | Accepted range |
| --- | ---: | --- |
| `Render.Lighting.Cluster.TileSize` | 64 pixels | 8..1024 |
| `Render.Lighting.Cluster.DepthSlices` | 32 | 1..128 |
| `Render.Lighting.Cluster.FarDepthCm` | 100000 cm | 1..1e9, finite |

FarDepthCm controls the logarithmic subdivision distance, bounded by the finite
camera far plane. It does not discard farther visible lights: the last cell
extends to the camera far plane, or infinity for an infinite camera.
Memory is `ceil(width/tile) * ceil(height/tile) * slices * ceil(lights/32) * 4`
bytes per mask buffer. At 1920x1080, 135 lights and defaults this is 326400 bytes
per buffer. More slices/smaller cells trade CPU work and upload bytes for tighter
light selection. Allocation overflow, invalid projections and nonfinite/invalid
light spheres are rejected.

## Verification and comparison

Build `Apps/App/LightingGpuTests.vcxproj` with VS 2026 MSBuild, x64 Release or Debug,
then run its executable from the repository root. The local MSVC PDB-server C1902
issue requires `_CL_=/Z7 /GL-` in this environment. A full rebuild is needed after
changing shared C++ layouts if an interrupted build left stale library objects.

CPU checks cover INI parsing and startup locking, byte-identical masks with one
and four threads, sphere-surface coverage, a rotated/translated camera, jitter,
variable/non-power-of-two cell sizes and slice counts, and infinite far depth.
325 GPU fixtures compare the production wave path, non-wave path and archived
previous shader against brute-force light evaluation. They exercise sky/flat/
layered/mixed/near-far depths, partial tiles, 0..769 sources, 32-bit word boundaries,
and the former 512/513 overflow boundary. HDR tolerance is 0.2% with a 0.002 floor
for floating-point summation order. The existing lighting smoke checks actual
point/spot PCF and AO energy under the D3D12 debug layer.

`Apps/App/tests/shaders/DepthAdaptiveLighting.hlsl` freezes the previous fused,
depth-adaptive GPU implementation for reproducible comparison only. Its constants
are historical baseline parameters, not production tuning. Benchmarks use identical
G-buffers, light data and shading. They report the median of five warmed batches at 320x180
and 1280x720 for 135/512 lights, dense and layered surfaces. CPU time includes grid
building, job completion, staging writes and command recording. GPU timestamps
include the new mask copy/barriers and shading; the baseline includes its GPU list
building and shading. Buffer allocation is warmed up. CPU/GPU times are reported
separately; their sum is not a measured application frame time or Bistro FPS.
Use `LightingGpuTests.exe --benchmark` to run timings without the debug layer;
the default invocation runs correctness checks with the layer enabled.

The [recorded comparison](../research/2026-11-02-frustum-grid-performance.md) shows
no overall speedup for these fixtures: CPU work increases and GPU time is similar
at 720p. Four threads reduce the new CPU cost by roughly 1.5–2.5x at that resolution.

## Sources

- [Scalarization part 1](https://flashypixels.wordpress.com/2018/11/10/intro-to-gpu-scalarization-part-1/)
- [Scalarization part 2](https://flashypixels.wordpress.com/2018/11/10/intro-to-gpu-scalarization-part-2-scalarize-all-the-lights/)
- [Drobot, CoD SIGGRAPH 2017, especially PDF page 17](https://www.realtimerendering.com/advances/s2017/2017_Sig_Improved_Culling_final.pdf)
- [Filament CPU froxel assignment](https://google.github.io/filament/Filament.md.html#lighting/directlighting/clusteredforwardrendering)

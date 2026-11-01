# Procedural 2D clouds — UNVERIFIED

Implementation only, 2026-11-01. No builds, tests, GPU fixtures, benchmarks or
separate review/verification passes were run, per the author's task contract.

`GDX12CloudComponent` is an explicitly consumed renderer component. Each instance
owns its parameters, animation phase, texture and pipeline. No scene installation,
entities, materials or assets are added. This is a 2D procedural background, not
volumetric clouds, atmospheric scattering, raymarching or geometry.

## Instance API

Call `SetParameters(GDX12CloudParameters)` on the render thread between frame
recordings. Invalid finite/range/implementation values throw before mutation.
`GetParameters()` returns author values, never rewritten by quality presets.
`Advance(simulationDeltaSeconds)` is called once per simulation tick by the owner,
with world time scale/pause already applied: pass zero while paused. Accepted dt
is finite [0, 3600] seconds. Off freezes phase. `Reset()` resets phase and records
a discontinuity. Multiple components can have independent parameters and clocks.

| Field | Default | Range / units |
|---|---|---|
| Enabled | true | Per-instance enable |
| Implementation | 0 | 0 simplex fBM, 1 simplex ridged |
| Octaves | 0 | 0 inherits quality; explicit 1..8 survives quality changes |
| Scale | 4 | 0.01..64, direction-space noise scale |
| Coverage | 0.5 | 0..1, exact clear/full endpoints |
| Softness | 0.15 | 0.001..1, normalized density threshold width |
| Speed | (0.02, 0.01) | Each -10..10 simplex lattice units / simulation second |
| HorizonColor | (0.35, 0.5, 0.8) | Each 0..64, linear HDR radiance |
| ZenithColor | (0.05, 0.15, 0.4) | Each 0..64, linear HDR radiance |
| CloudColor | (2, 2, 2) | Each 0..64, linear HDR radiance |

Only two global CVar exist: `Render.Clouds.Enabled=true` and
`Render.Clouds.Quality=2`, runtime Default, registered in the component .cpp for
ConsoleModule and startup Engine.ini. Off/Low/Mid/High resolve inherited octaves
to 0/3/5/8. Explicit octaves remain independent. Both implementations execute
different accumulation formulas; quality changes actual shader work.

## Explicit sky and material consumers

The scene/sky owner calls `environment.SetCloudSource(&cloud)` and advances that
instance before executing the environment pass. The environment sets up and
executes the selected source on its device. `nullptr` or component/global Off
restores the existing HDRI. The single selected source feeds background,
irradiance and GGX filtering; no second sky is drawn. The source pointer is
non-owning: detach before destruction and retire GPU work before releasing either
pass. A stable output-3 link resolves the active source per frame so existing
lighting/probe consumers do not need relinking.

For an explicit material consumer, call `Setup` with its device, advance once per
simulation tick, and `Execute` before drawing. Output 0 is a linear RGBA16F
TextureCube SRV sampled by direction, not a 2D diffuse texture. Material shaders
can also include `CloudNoise.hlsli` and call its 2D functions with their existing
instance uniforms. Existing opaque 2D texture slots cannot accept this cube.
Off cube output contains clear sky RGB and zero alpha; compositing callers must
honor alpha. A component cannot migrate device-resource owners after allocation.
Do not execute the same instance with different snapshots in one command list.

Construction/Setup/Initialize/Advance allocate no GPU resources. Explicit Execute
lazily creates one 256x256x6 single-mip RGBA16F cube (3 MiB), two descriptors and
a root/pipeline. Quality changes octaves, not resolution. Descriptors and resources
are stable across settings changes and resize. Queued frames use one ordered
graphics queue, root constant snapshots and UAV-to-SRV barriers. Terminal
`ClearDenendencies` follows the existing pass teardown contract: retire work first.
The environment reuses its filter textures and generates the DFG LUT only once.

`GetSourceRevision()` tracks author edits, reset and rendered quality/enable
changes. `GetLightingRevision()` additionally tracks continuous animation. Source
changes increment ReflectionEnvironmentRevision for probes/SSSR. Animation updates
environment filtering without resetting temporal histories each frame. Baked
probes retain their snapshot during continuous animation; dynamic probes follow
their existing budgets. Paused unchanged components dispatch no cloud work.

## Pipeline seams

Generation/filtering run at the existing environment stage before prepass and
shadows/probes. No opaque/masked geometry is introduced; both AO prepass paths,
CSM/local shadows, probe geometry and object motion retain their existing paths.
Probe sky capture reads the same selected source. Deferred HDR background and IBL
share that source. SSSR/local fallback and the single specular resolve are
unchanged. WBOIT HDR -> FidelityFX DoF -> motion blur -> HDR XeSS -> one tone map
keeps its order. Existing background depth gives sky camera rotational motion;
noise animation has no separate geometry velocity. RH/reverse-Z/cm, positive
raster jitter, camera/history/resize and frame-slot contracts are unchanged.

## Provenance and unverified limits

MGPU-Noise/CloudsNoiseCS.hlsl was read for capability comparison. Its Shadertoy
URLs do not establish a source redistribution license. No donor code was copied.
CloudNoise.hlsli is an original implementation of standard simplex lattice
geometry using an integer periodic gradient hash and normalized fBM/ridged sums;
no third-party noise notice is required. Three direction-space 2D projections
blend without longitude seams or pole division. Phase wraps in that same periodic
simplex lattice, avoiding long-running float-time growth.

Single-mip cubes can alias at extreme Scale/Octaves. Animated sources rebuild
irradiance/GGX filtering every frame and may be expensive. There are no cloud
shadows, volumetric depth or cloud animation motion vectors. Central verification
must cover C++/HLSL compilation, seams/poles/finite values, two independent
components, Off/reset/pause/parameter/quality transitions, queued slots, source/IBL
consistency, GPU validation and timings. None is claimed passed.

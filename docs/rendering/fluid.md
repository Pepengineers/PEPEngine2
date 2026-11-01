# GPU fluid components and water surface

Актуальная интеграция: код объединён в корневую ветку и прошёл CPU/GPU/GBV smoke checks. [Результаты и непроверенные области](second-group-integration.md). Авторский статус ниже — исторический, до центральной интеграции.


Status: **UNVERIFIED implementation** (2026-11-01). No build, shader compilation,
test, GPU fixture, benchmark or separate review was run by the author, as directed.
CPU all-pairs/NRF oracles and GPU/readback acceptance belong to the central checker;
no verification infrastructure is added here.

`FluidRenderComponent` is a WorldECS component. Each instance owns its author
settings and immutable seed payload; its GPU particles, density, sorted keys,
bucket offsets and frame-slot constants are independent. Components do not exchange
particles or forces. No fluid, entity, model or emitter is added to Bistro.

## API and asset ownership

```cpp
// Configure an existing entity in application code. This is API usage, not a scene edit.
auto& fluid = world.GetECS().Get<FluidRenderComponent>(entity);
auto settings = fluid.GetSettings();
settings.ContainerHalfExtentCm = Vector3(100, 150, 100);
settings.SimulationEnabled = true;
settings.RenderEnabled = true;
fluid.SetSettings(settings);                 // validates before replacing author state
fluid.SetParticles(std::move(seedParticles)); // validates, snapshots, requests reset
fluid.Reset();                               // restore the current seed on the next enabled frame
```

`SetParticles(std::vector<GDX12FluidParticle>)` is the asset seam: the existing
asset/application owner supplies decoded local-space Position (cm) and Velocity
(cm/s). The component holds a shared immutable vector, so ECS frame snapshots do
not duplicate the payload or retain a raw pointer into a mutable asset. Copying a
component shares only its seeds, never GPU solver state. Empty seeds allocate no
GPU arenas. The limit is 1,048,576 particles per component. Seed positions and
velocities must be finite with magnitude <=10,000,000 in their respective units;
transformed seed positions and the container corners must remain within
10,000,000 cm of the world origin. There is no new asset registry or file format,
and no implicit SceneAsset/Bistro import of fluid components. Persistence follows
the existing runtime-only component API pattern, like reflection probes.

`TransformComponent` defines the container-to-world affine transform, including
rotation and nonuniform scale. Physical smoothing/render radii stay in world cm;
seed positions and seed velocities are transformed once on reset. Moving the
container moves its collider, not the entire simulated fluid. GPU collisions
transform positions into container space, shrink each local half-space by the
world sphere support, and reflect only outward world velocity along the inverse
transpose normal. The container must be invertible and large enough to contain a
particle after scale. The collider is the component's oriented box; mesh collision
and inter-component collision are not implemented.

The renderer copies components after ECS work in `RenderModule::CollectReflectionScene`
into `GDX12DeviceResources::Fluids`, with scene/entity identity, container matrix
and world-scaled frame delta (zero when the world is paused). Scene topology
revision changes reset identity as in the existing probe snapshot. Callers that
populate this vector directly must provide unique Ids, valid transforms and a
finite DeltaSeconds in [0,3600]. `ValidateFluidInstance` is also called by the pass.
Change component settings between frame snapshots, on the ECS update owner thread.

## Two global controls

| CVar | Default | Range / behavior |
| --- | --- | --- |
| `Render.Fluid.Enabled` | true | Runtime Default bool; suspends both solver and rendering globally |
| `Render.Fluid.Quality` | 2 | Runtime Default int: 0 Off, 1 Low, 2 Mid, 3 High |

Both register once in `GDX12Fluid.cpp`, before startup INI loading, and are available
through ConsoleModule and `Config/Engine.ini`. No other fluid CVar exists.

Zero on the three budget fields below means inherit. Resolved Low/Mid/High values
are MaxSubsteps 16/32/64, FilterIterations 1/2/3, FilterRadiusPixels 8/16/32.
`ResolveFluidSettings` returns a copy: quality changes never overwrite author
values; a nonzero budget remains explicit until the caller sets it back to zero.
There is one real implementation (SPH + NRF), so no unused Implementation enum.

## Per-instance fields

All fields are in `GDX12FluidSettings`, changed with GetSettings/SetSettings.
Invalid finite/range values throw without changing the component.

| Field | Default | Units and accepted range |
| --- | --- | --- |
| SimulationEnabled / RenderEnabled | true / true | Independent bools: simulate invisibly, or render a frozen solver |
| MaxStepSeconds | 1/240 | seconds, [0.000001,0.05] |
| MaxSubsteps | 0 | 0 inherit, otherwise [1,64] |
| TimeScale | 1 | [0,8]; zero freezes solver |
| SmoothingRadiusCm | 4 | world cm, [0.1,1000] |
| ParticleRadiusCm | 1 | world cm, [0.01,SmoothingRadiusCm] |
| ParticleMassKg | 0.008 | kg, [1e-9,100000] |
| RestDensityKgPerCm3 | 0.001 | kg/cm^3, [1e-8,100]; default is 1000 kg/m^3 water |
| PressureStiffnessCm2PerSecond2 | 40000 | cm^2/s^2, [1,1e8]; pressure = stiffness * max(density-rest,0) |
| ViscosityCm2PerSecond | 1 | kinematic cm^2/s, [0,100000] |
| AccelerationCmPerSecond2 | (0,-981,0) | world cm/s^2, finite, magnitude <= MaxAcceleration |
| MaxSpeedCmPerSecond | 1000 | safety/CFL speed cap, [1,100000] |
| MaxAccelerationCmPerSecond2 | 100000 | safety/step acceleration cap, [1,1e8] |
| ContainerHalfExtentCm | (100,100,100) | local cm, each (ParticleRadiusCm,1e6], plus transformed sphere-fit validation |
| Restitution | 0.2 | [0,1] |
| FilterIterations | 0 | 0 inherit, otherwise [1,16]; each iteration is X then Y |
| FilterRadiusPixels | 0 | 0 inherit, otherwise [1,64]; maximum 1D radius in render pixels |
| FilterSigmaCm | 0.7 | world Gaussian sigma, [0.001,1000] |
| FilterDepthRangeCm | 10 | NRF delta in eye cm, [0.001,100000] |
| FilterEdgeCm | 1 | NRF mu in eye cm, [0,FilterDepthRangeCm] |
| IOR | 1.333 | [1,3], air-to-fluid interface |
| AbsorptionPerCm | (0.015,0.004,0.002) | RGB Beer-Lambert coefficient per cm, each [0,100] |
| ScatteringColor | (0.02,0.08,0.1) | linear HDR RGB, each [0,100] |
| Roughness | 0.05 | environment/probe prefilter LOD, [0,1] |
| RefractionPixels | 12 | projected Snell ray displacement cap, render pixels [0,128] |
| MaxOpticalThicknessCm | 1000 | summed optical path cap, [0.01,1e6] cm |

`Reset` increments a generation; it is consumed on the next enabled use, without
turning either local enable flag on. Reset/source replacement/removal flushes
queued GPU users before replacing resources or descriptor slots. Per-frame
parameter uploads use the existing waited frame ring, not a mutable shared CBV.
Particle buffers are shared across frames only on the ordered graphics queue.
Disabling rendering releases surface targets after waiting; simulation state is
retained to resume. Removing/emptying a component releases its GPU state. No
solver/viewport buffers are allocated for an initially empty or disabled component.

## Solver and surface algorithms

The solver independently implements poly6 density, symmetric pressure gradient,
viscosity Laplacian and semi-implicit integration from
[Muller, Charypar and Gross 2003](https://matthias-research.github.io/pages/publications/sca03.pdf).
The donor project was read only for its known failure modes; its framework and
sort implementation were not copied.

Every substep builds a power-of-two bucket array, bitonic-sorts (bucket,particle)
on the GPU and constructs bucket start offsets. All 27 queried cells compare
full int3 coordinates after hash lookup, so hash collisions neither add unrelated
particles nor duplicate a neighbor. Density is computed to its own buffer before
integration reads it. Integration reads immutable input positions/velocities and
writes the other particle buffer. UAV barriers separate passes/sort stages.
Coincident pairs use a deterministic antisymmetric direction. Self-density is
included and density divisors have a rest-relative positive lower bound.

The timestep is bounded by MaxStepSeconds, 0.25*h/(MaxSpeed+sqrt(stiffness)),
0.25*sqrt(h/MaxAcceleration), and (when viscosity>0) 0.125*h*h/viscosity.
Frame delta * TimeScale is split into at most MaxSubsteps of this safe size;
excess elapsed time is dropped without accumulating a catch-up backlog. Safety
caps can deliberately change the physical solution, and low budgets can slow
simulation relative to wall time. They are explicit component fields. Nonfinite
integration results retain the previous finite position and zero velocity.

The renderer rasterizes analytic sphere intersections into nearest eye depth and
particle displacement, with a separate additive clipped chord-thickness draw.
It then applies independent
[Truong/Yuksel 2018 narrow-range filtering](https://www.cemyuksel.com/research/papers/narrowrangefilter.pdf):
foreground rejection, far-depth clamping, opposing-pair bias correction, dynamic
range expansion, projected world-space sigma, alternating 1D passes and a final
5x5 2D cleanup. This is a paper implementation, not the all-rights-reserved
reference code. World normals come from the filtered depth with shorter one-sided
differences near edges. The nearest surface across components owns the pixel's
normal, thickness, depth and motion; each owner resolves its own optics.

Resolve uses Snell refraction projected back to screen, rejects opaque foreground
samples, uses RGB Beer-Lambert absorption/scattering, exact unpolarized dielectric
Fresnel, global prefiltered environment and the existing box-projected local-probe
blend. There is no second opaque-specular contribution. Screen-space particle
thickness is an additive optical approximation; it is not a watertight volume.

## Render seams

- Main: deferred HDR -> existing AMD SSSR/denoiser/opaque specular resolve -> fluid
  simulation/sphere/NRF depth -> behind-water WBOIT + HDR composition -> water HDR
  optics -> front-water WBOIT + HDR composition -> FidelityFX DoF -> feature-aware
  motion blur -> HDR XeSS -> the existing single tone map.
- Both WBOIT groups use the existing GPU-culling ExecuteIndirect geometry lists.
  They split each glass fragment against nearest filtered water depth and opaque
  depth. The front pass reuses the behind MRTs after water resolve. Composition
  reuses the behind HDR target after its contents have been consumed by resolve.
  With no water, front draws/composition bypass and allocate no second MRT set.
- WBOIT reads opaque depth through a read-only DSV and a combined DEPTH_READ |
  PIXEL_SHADER_RESOURCE state. It does not overwrite opaque or water depth.
- Prepass: water is transparent screen-space geometry, so it does not enter either
  opaque/masked prepass variant, G-buffer Equal draws, AO or opaque SSSR inputs.
  Existing depth-only/normal prepass ordering is preserved.
- Shadow/probe seam: existing opaque/masked CSM/local shadows and probe capture are
  untouched. Water samples published local probe radiance; like existing glass,
  water is not captured by probes and does not cast opaque shadow maps. This
  implementation does not add water caustics, direct-light highlights or mesh
  shadow geometry.
- Motion: save previous particle position once per rendered frame before substeps.
  Reconstruct current smoothed world position with jittered depth; subtract its
  particle displacement for previous world position; project with current/previous
  unjittered matrices and store prevNDC-currentNDC. Camera cuts, resize, reset,
  visibility/settings/global transitions suppress stale motion/history. DoF,
  motion blur and XeSS consume the combined nearest water/opaque depth and motion,
  on exactly the HDR render grid. Raster jitter remains the engine's positive jitter.
- Asset seam: only immutable CPU seed data and existing ECS snapshots. No Bistro
  content or external runtime/LFS payload is modified.

Multiple solver instances work independently. Screen-space composition represents
one nearest water interface per pixel. Overlapping/nested fluid interfaces do not
receive recursive refraction through each other; thickness belongs to the nearest
component. WBOIT still approximates glass ordering within each side of that
interface, and glass itself retains the baseline transparent depth/motion limits.
Near-plane-intersecting spheres use a conservative fullscreen bound and clipped
chords; an underwater camera does not have a complete two-sided volumetric model.

The shared descriptor allocator now throws on an exhausted range instead of
walking outside the range/heap. This is needed for multiple independently sized
fluid instances; no new descriptor arena or global allocator was introduced.

All behavior, compilation, GPU stability, optics, transitions, timings and memory
costs above remain **UNVERIFIED**. No FPS, image-quality or long-run claim is made.

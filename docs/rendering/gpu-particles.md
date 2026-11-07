# GPU particle components

Актуальная интеграция: код объединён в корневую ветку и прошёл CPU/GPU/GBV smoke checks. [Результаты и непроверенные области](second-group-integration.md). Авторский статус ниже — исторический, до центральной интеграции.

Optional particle async реализован 2026-11-05, default-off: [проверки, консервативный depth join, timings и чистый полный Bistro GBV после SSSR counter fix](../research/2026-11-05-particle-async-compute.md).


Implementation status: **UNVERIFIED / НЕПРОВЕРЕНО**. No builds, shader compilation,
tests, GPU fixtures, benchmarks or separate review/cleanup/verification passes were
run for this implementation, per the implementation-only instruction. The checks
in the task package remain the central reviewer's acceptance criteria.

## Ownership and API

`GDX12ParticleSimulation`, owned by device resources, owns live GPU pools and
simulation pipelines independently of `GDX12ParticlesPass`. At frame preparation,
`BeginFrame` validates/snapshots emitters and allocates resources; the draw pass's
`BeginFrame` prepares only rendering resources. `InitializePools` records pool
initialization/reset before the render graph. `GDX12ParticleSimulationPass` then
advances all emitters immediately after the full opaque depth prepass, before
early AO and shadows. Its depth input excludes water and transparent geometry.
The late particle pass only reads published pools for culling, sorting, color and
motion. The graph declares a separate simulation dependency and each frame's
physical persistent pool accesses. `Render.Particles.AsyncCompute` defaults to
false. With it enabled on a single GPU, simulation uses a COMPUTE queue; queue
creation failure and dual-GPU mode keep the DIRECT path. Initialization/reset,
cull/sort/draw and composite remain on DIRECT.

The graph releases initialized pools through COMMON and transitions opaque depth
to NON_PIXEL_SHADER_RESOURCE before the fork. COMPUTE waits on DIRECT; DIRECT joins
before the first dependent pass or any later access to that depth texture, including
readers. Depth-independent shadows/probes can overlap simulation; early AO joins
immediately. Concurrent texture readers are deferred after native D3D12 validation
also reported cross-queue hazards in the stress fixture. A simulation-only branch also
joins before frame retirement. The final DIRECT fence covers frame constants,
uploads and descriptors; compute list/allocator and timestamp slots use their
own queue fence. The next frame's DIRECT prefix follows the preceding graphics
readers, protecting pool reuse without adding a frame of simulation latency.

The separation was checked on 2026-11-04 with Release/x64 builds,
`SecondGroupTests` (debug layer and `--gpu-validation`) and
`BistroRenderGraphTests` (both AO placements, pooled/dedicated resources, resize,
native/XeSS switching). Particle readbacks cover hidden simulation progression
and expiry, simulation freeze, master disable, pending bursts and reset while
both stages are disabled. Performance and long-run stability were not measured.

`ParticleEmitterComponent` is a WorldECS component. A component needs an existing
entity's `TransformComponent`. Bistro installs eight App-owned effects with a shared
`App.Particles.CountScale` multiplier; see [scene techniques](scene-techniques.md). Each emitter has its own GPU pool, counters, random stream, author settings,
reset revision and pending burst requests. Component copies get a new identity;
ECS moves preserve identity, so dense-pool compaction does not reset simulation.
Copying a component copies author settings and burst requests, not live GPU state.

```cpp
auto& emitter = world.GetECS().Emplace<ParticleEmitterComponent>(existingEntity);
auto settings = emitter.GetSettings();
settings.MaxParticles = 32768;
settings.Blend = ParticleBlend::SortedAlpha;
settings.ColorStart = Vector4(4, 1, 0.2f, 0.8f);
settings.Texture = renderModule.GetTextureByName("existing_particle_atlas");
emitter.SetSettings(settings);
emitter.Emit(1000);
```

This is an API example, not code executed against Bistro. Two components can use
different settings and materials; sorted-alpha particles from all components share
one GPU key/payload stream and one indirect draw. Update the component during scene
update, before rendering's immutable per-frame snapshot; concurrent mutation during
collection is unsupported, as for the existing ECS components.

- `GetSettings()` returns a const reference; edit a copy and call `SetSettings()`.
  Invalid finite/range/enum combinations throw `std::invalid_argument`, retaining
  the old settings. Author settings are never overwritten by presets.
- `Emit(count)` queues a burst, 0..1,048,576 particles per call. Repeated calls add
  requests with checked 64-bit overflow. Requests remain pending while paused.
  On an active frame, available pool slots bound the accepted burst plus continuous
  emission; overflow is dropped without backlog or a CPU count readback.
- `SetPaused(bool)` freezes age, position and emission while particles remain
  visible and retain camera motion. World pause also freezes emission. On resume,
  no missed wall time is simulated. `TimeScale=0` stops continuous simulation while
  still allowing an explicitly requested burst.
- `Reset()` clears live state, emission fraction and random sequence on the next
  GPU frame, including queued bursts. Calling `Emit()` after `Reset()` spawns into
  the reset pool. Reset works while paused. Render history is invalidated.
- `SetEnabled(false)` removes this component from rendering; its pool is retired
  at the next enabled particle frame. Re-enable starts a new pool. Global Off
  freezes all retained pools and suppresses rendering; re-enable resumes them.
- Moving an emitter changes only future spawn positions/directions. Existing
  particles simulate in world space. Shape is transformed by the entity's full
  scale/rotation/translation; velocity direction uses rotation/scale then normalizes
  so the authored speed remains cm/s. Size is authored in world cm.

The API is runtime C++; no scene-file particle serialization/editor UI is added.
`RenderModule::CollectParticleEmitters` snapshots components from all loaded worlds,
using world pause/time scale. Nonfinite/negative frame time and nonfinite/singular
world transforms are rejected before dispatch. Negative world time is unsupported.
`GDX12DeviceResources::ParticleEmitters` is also the renderer's direct snapshot seam;
direct callers must provide unique nonzero stable IDs and the same valid data.

## Global controls and quality

Three independent bool controls (all default true) are registered in
`GDX12Particles.cpp` and startup `Config/Engine.ini`:

- `Render.Particles.Enabled`: master switch; false suppresses both stages and
  retains existing pools without advancing time.
- `Render.Particles.DrawEnabled`: false skips culling, sorting, color and motion
  output while simulation and spawning continue. Restoring drawing preserves the
  live pool and invalidates render history.
- `Render.Particles.SimulationEnabled`: false freezes motion, age and spawning
  while existing particles remain drawable. Pending `Emit` requests wait for
  resume, without elapsed-time catch-up. `Reset` still clears the pool immediately
  during frame initialization, including when drawing is also off.

`Render.Particles.Quality` retains budgets 0 Off, 1 Low, 2 Mid (default), 3 High.
Quality Off acts like the master switch. Per-emitter/world pause still applies.

| Inherited field | Low | Mid | High |
|---|---:|---:|---:|
| MaxParticles = 0 | 16,384 | 65,536 | 131,072 |
| Substeps = 0 | 1 | 2 | 4 |
| EdgeSamples = 0 | 1 | 2 | 4 |
| MinPixelSize = -1 | 1.5 px | 0.75 px | 0 px |

Explicit values survive quality changes. Returning an individual field to its
sentinel restores inheritance. Changing resolved capacity resets only that pool;
changing samples/substeps/culling threshold does not reset it. EdgeSamples changes
atlas supersampling; an analytic disc uses derivative-based edge coverage.
One scene is limited to 4,194,304 pool slots in total, checked before allocation.

## Author settings

All numeric fields must be finite. Distances are cm, time is seconds, speeds cm/s,
acceleration cm/s², rotation speed rad/s. Color is linear HDR, straight alpha.

| Field | Default | Accepted values / meaning |
|---|---|---|
| MaxParticles | 0 | 0 inherit, otherwise 1..1,048,576; changing resolved value resets pool |
| Substeps | 0 | 0 inherit, otherwise 1..16 simulation steps per frame |
| EdgeSamples | 0 | 0 inherit, otherwise 1..4 atlas samples per pixel |
| MinPixelSize | -1 | -1 inherit, otherwise 0..64 projected pixels for size culling |
| EmissionRate | 100 | 0..10,000,000 particles/s; GPU fractional carry |
| LifetimeMin / LifetimeMax | 1 / 2 | 0.001..3600 s, max >= min |
| SpeedMin / SpeedMax | 20 / 100 | 0..1,000,000 cm/s, max >= min |
| Direction | (0,1,0) | each -1..1, squared length > 0.000001; normalized in shader |
| ConeAngleDegrees | 30 | 0..180 degrees half angle; uniform solid-angle distribution |
| Shape | Point | Point, Box or Sphere; sphere samples volume uniformly |
| ShapeExtents | (10,10,10) | each 0..1,000,000 cm; box half-extents or ellipsoid radii |
| Acceleration | (0,-98.1,0) | each ±1,000,000 cm/s², world space |
| Wind | (0,0,0) | each ±1,000,000 cm/s, drag target velocity, world space |
| Drag | 0 | 0..1000 s⁻¹; exponential velocity damping with acceleration |
| SizeStart / SizeEnd | 10 / 0 | 0..1,000,000 cm, full billboard width over life |
| SizeVariation | 0.25 | 0..1, uniform size multiplier 1±variation per spawn |
| RenderScale | 1 | 0.001..100, billboard size multiplier; does not change simulation or internal resolution |
| ColorStart / ColorEnd | (1,1,1,1) / (1,1,1,0) | RGB 0..100, alpha 0..1, interpolated over life |
| AngularVelocity | 0 | ±10,000 rad/s, random initial billboard rotation |
| SoftDistance | 10 | 0..1,000,000 cm; 0 hard intersections |
| MaxDistance | 100,000 | 1..100,000,000 cm positive view distance; capped by camera far |
| MaxDeltaTime | 0.1 | 0.001..1 s, clips scaled frame delta; dropped time is not accumulated |
| TimeScale | 1 | 0..100, multiplied with world scale before MaxDeltaTime clamp |
| Seed | 1 | full uint32 range; Reset restarts sequence with the current seed |
| Blend | WeightedAlpha | WeightedAlpha, Additive or SortedAlpha |
| Geometry | Billboard | Billboard, WaterDrop, or volumetric Firefly; only Firefly excludes SortedAlpha |
| Texture | nullptr | renderer-owned primary-GPU color 2D texture; nullptr = white analytic disc |
| AtlasColumns / AtlasRows | 1 / 1 | each 1..256, no more than texture width/height |
| FramesPerSecond | 0 | 0 = atlas once over life; otherwise 0..1000 fps, looping |
| DepthCollision | false | Enable screen-space collision against the current scene depth |
| CollisionRadius | 1.5 | 0.01..1,000,000 cm; sphere radius independent of billboard size |
| CollisionRestitution | 0.45 | 0..1; normal velocity retained after bounce |
| CollisionThickness | 10 | 0..1,000,000 cm; maximum tolerated initial penetration |

The texture is obtained through existing `RenderModule::CreateTexture` or
`GetTextureByName`, uses its existing sRGB color SRV and material descriptor range,
and must outlive the component/snapshot. No new asset registry or material owner is
introduced. Atlas cells should have padded edges/mips to prevent mip bleeding.
Materials are unlit emissive/tinted sprites, with analytic discs or color/alpha
flipbooks; mesh PBR materials are not reinterpreted as particle materials.

## GPU cycle and resource lifetime

Each pool uses 64 bytes per particle, two uint alive lists, one uint dead list,
eight uint counters and three dispatch argument triplets. One dispatch initializes
the dead permutation. Simulation consumes the previous alive list and compacts
survivors into the next list; deaths append indices into the dead list. A one-thread
GPU stage reserves `min(requested, deadCount)` spawn indices, subtracting only a
bounded value. Spawn appends to the same alive output. GPU publish writes the alive
count and cull dispatch. CPU only swaps the two list references, never reads counts.
The invariant is `aliveCount + deadCount = capacity` after publish, with unique pool
indices; no decrement/consume on a zero dead count is used.

UAV dependencies separate initialization, simulation/death, spawn reservation,
spawn, publish, cull, argument generation and each sort stage. Argument buffers
transition between UAV and INDIRECT_ARGUMENT. Pool constants are uploaded into the
current fenced frame slot. GPU-only buffers are shared sequentially on the existing
direct queue; no readbacks participate in continuation. Pool replacement/removal
and descriptor reuse flush previous submissions before recording particle work.
Resize recreates only the coverage/motion target after the existing resize flush;
live particle pools survive. Reset/re-enable/resize invalidate the upscaler history.

The shared visible record stream uses 96 bytes per slot; two unsorted index regions
hold weighted/additive references. Sorted emitters use the same draw-record stream,
two uint key buffers and two uint payload buffers. GPU-generated sorted count and
dispatch distribution feed the **actual pinned FidelityFX Parallel Sort kernels**
through eight 4-bit count/reduce/scan/scan-add/scatter passes. There is no CPU sort
count or CPU sorting. Positive view distance keys are inverted for far-to-near
order. Zero visible count still produces bounded empty work and zero draw instances.
SM6 wave operations with minimum wave width >=16 are required for sorted alpha;
unsupported devices fail explicitly instead of silently giving unsorted alpha.
Source/license pointers are in `External/Particles/README.md`.

No pool/draw arena, motion texture, sort scratch, particle shader or PSO is created
for a scene without enabled particle components. Sort scratch/kernels are lazy and
only requested for SortedAlpha. Disabling the last component retires all particle
resources; global Off retains existing state without allocating new resources.

## Rendering seams and limits

Billboards already use quad instancing, with no geometry shader: `ParticleRaster.VS`
uses `SV_VertexID` for six triangle-list corners and `SV_InstanceID` for each
visible particle. GPU-generated `D3D12_DRAW_ARGUMENTS` are submitted with
`ExecuteIndirect`; all four billboard PSOs bind only VS and PS. For lifetime atlas
playback (`FramesPerSecond=0`, also used by Bistro), frame selection is
`min(Columns*Rows-1, floor(Age/Lifetime * Columns*Rows))` in the compute cull pass.
The pixel shader samples that frame's cell from the packed texture.

`ParticleGeometry::WaterDrop` keeps the six-vertex billboard path and all three
blend modes. It draws a 2.5x vertical droplet silhouette with spherical normals,
water IOR 1.333 Fresnel reflectance, stepped blue shading, a dark blue rim and
a bright highlight. WeightedAlpha preserves semi-transparency without additive
emission. The core retains 70% of authored alpha instead of becoming invisible.
Culling includes the elongated silhouette. Transmission uses alpha compositing,
not a scene-color refraction lookup; large close-up lenses need a different path.

`ParticleGeometry::Firefly` selects a shared immutable vertex buffer: five closed
octahedra form a head, body, emissive abdomen and two volumetric wings, with
120 vertices / 40 triangles (below the 600-vertex limit). `MeshVS` uses normal
vertex input and `SV_InstanceID`; there is no GS or mesh-shader stage. Wings flap
by particle age and the whole model rotates in world space. Previous age/rotation
also animate the previous vertices for motion output. Material colors and a fixed
key light show the facets; this is stylized emissive shading, not scene PBR lighting.

Mesh weighted/additive lists use separate GPU counts and indirect arguments;
ordinary sprites still draw six vertices, so adding meshes does not inflate their
vertex work. Mesh resources and three PSOs (weighted/additive/motion) are lazy.
The shared 96-byte draw record remains unchanged in size. Additional index regions
cost eight bytes per scene slot only when mesh particles are present. Geometry
fits the existing culling sphere. SortedAlpha mesh settings are rejected explicitly;
weighted and additive meshes work alongside all three billboard blend modes.

Order: particle pool initialization/reset → existing depth prepass → particle
simulation → early AO if selected → CSM/local shadows/probes →
opaque G-buffer/late AO → deferred → AMD SSSR + single specular resolve → mesh
WBOIT → particle cull/sort + weighted accumulation → WBOIT HDR resolve →
global sorted-alpha particle draw → additive particle draw → DoF → feature-aware
motion blur → HDR XeSS → the existing single tone map.

- `GDX12ParticlesPass` runs after mesh WBOIT and appends to its two HDR targets.
  `GDX12WBOITCompositionPass::SetParticles` calls `Composite` on the resolved HDR
  target. Additive premultiplied radiance uses ONE+ONE directly on HDR, preserving
  target alpha; it never enters normalized WBOIT accumulation with zero alpha.
- Sampled scene depth has PIXEL_SHADER_RESOURCE state and **no DSV is bound** for
  particle draws. Reverse-Z depth rejection and positive linear-depth soft fading
  use the same jittered depth grid. Billboards use the current jittered ViewProj.
- Particles are transparent, do not write opaque depth/normals and do not alter
  either prepass path. They are not opaque/masked mesh geometry or shadow/probe
  casters. Existing CSM/local shadow, probe capture, AO timing and SSSR fallback
  paths remain unchanged. No second specular resolve or tone map is introduced.
- A lazy RGBA32F motion/coverage target stores alpha-weighted motion and depth
  for all modes without half-float counter overflow. Motion is unjittered
  `prevNDC-currentNDC`, including particle displacement, size, rotation and previous
  camera-facing axes. Camera resets zero motion. `ParticleMotion` is a borrowed
  per-frame device-resource pointer, null when there is no particle output.
- Motion blur mixes this motion by coverage after its sky-motion preparation;
  dominant coverage uses particle depth in feature-aware gathering. Its complete
  motion output continues to XeSS. XeSS still receives opaque depth. DoF also uses
  the existing opaque depth, as for mesh WBOIT transparency.

Sorted alpha is globally ordered **among SortedAlpha emitters**, not interleaved
with weighted mesh/particle transparency. Weighted particles share approximate
WBOIT ordering with transparent meshes. Additive energy is drawn after alpha modes.
Motion/depth of overlapping transparent layers is a coverage-weighted approximation,
not an exact per-layer history/depth solution. Transparent DoF/refraction, lighting
and shadow casting, arbitrary imported particle meshes, GPU child/subemitters,
multi-GPU and SSAA are outside this implementation. Very large HDR overlap can
overflow existing RGBA16F WBOIT/HDR targets; practical overlap budgets require GPU
measurement. No visual quality, GPU timing, debug-layer cleanliness, build success
or long-run stability is claimed.

Depth collision reconstructs a local surface from current jittered reverse-Z
opaque prepass depth and its closest depth neighbours. It runs per simulation substep, corrects sphere
penetration and reflects incoming velocity. It sees only the frontmost on-screen
depth, excluding water and other transparent surfaces; hidden/off-screen geometry
and glass without depth writes do not collide. Silhouette and fast grazing hits can be missed.

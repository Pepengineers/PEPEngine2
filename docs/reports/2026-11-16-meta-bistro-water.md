# Bistro water / SPH in ordinary Meta

## Scene and controls

Base at task start: f6d167f3 (rendering base 9fe0fed8); probes are absent.
Only BistroWater::Scene is added beside existing scene additions. It reuses the
stone basin, original materials and placement: center (-1297,65.3,-425) cm,
half-extents (260,38,132), yaw 135 degrees. Existing SparkRain shares the same
layout and is not duplicated. The helper creates/destroys container and fluid
entities only when Count changes, using existing dirty journals.

Meta Startup explicitly selects Count=1, ParticleCount=100000,
PresetParticleCount=0, HeightScale=2, WaveSpeed=6, WaveAcceleration=4,
WavePeriod=6, ParticleRadius=0.15, FilterRadiusPixels=6, Fluid.Enabled=true,
Quality=2 (Mid), AsyncCompute=false and SecondaryAdapter=-1. Source defaults and
saved App override (HeightScale=2) agree. Solver ReferenceModel, seed 0xB15720,
mass=1, rest density=630, H=0.2, pressure=288, near-pressure=2.15, viscosity=0,
prediction=1/120 and restitution=0.95 are unchanged. The created component logs
100980 seed particles for target 100000, following the original grid rounding.

Simulation stays in the original container-local units. The entity transform
converts to world centimetres; HeightScale also shifts the center upward so the
floor stays fixed. Virtual walls above the basin rim remain App behavior; no
overflow or rain/SPH/body coupling is added. Physics/Jolt stays paused.

BistroWaterRendering::CollectScene has an ordinary-only opt-in to ignore world
pause. Default App/diagnostic calls retain their previous pause behavior. World
time scale, component TimeScale, simulation/render flags and ResetRevision remain
in the existing snapshot/PlanSteps contract. MaxStep=1/180 and MaxSubsteps=3 bound
each frame; one snapshot/simulation advances once, independently of views/slots.

## Ownership and composition

The ordinary water owner records existing GDX12FluidSurfacePass/solver stages on
DIRECT through Meta. Solver pools/head/seed and previous displayed positions
persist across frames. Rare replacement/reset/destruction retires prior GPU use
through the executor callback. Screen resize changes surface targets/history,
without replacing the solver or reseeding. No private recording submit/Flush,
particle readback or per-frame static Bistro upload is introduced.

Granular simulation, surface/filter/merge and optics accesses describe actual
reads/writes. Surface follows opaque mesh/grass geometry before ordinary lighting;
lighting and SSSR still use opaque GBuffer/depth. The active composition is:

1. Far mesh WBOIT clear/draw and resolve.
2. Water optics resolve over that background, with the existing active HDR copy,
   refraction/absorption/scattering/Fresnel and static IBL reflection.
3. Near mesh WBOIT clear/draw, weighted particle append, one near resolve.
4. Global SortedAlpha/additive particles, Motion Blur/complete motion,
   reconstruction/native, display, window-sized UI and PRESENT.

An explicit optics → near draw dependency retains the phase order. Each mesh
phase clears once; particles do not clear WBOIT. One persistent SceneParticles
owner keeps its burst consumption, global A/B sorting, blends, wave compaction
and DEFAULT Firefly vertices. No diagnostic particle simulation or global owner
swapping runs alongside it.

Particle simulation/depth collision reads opaque depth; raster/soft fade reads
merged R32_FLOAT water depth. Particle raster uses a shader-only depth mode fixed
before PSO initialization, so merged SRV/UAV is never bound as DSV and enable/
bypass transitions need no PSO replacement. No depth writes are added.

Disabled/absent/non-rendered water returns opaque depth/motion and the All
transparency path without stale surface sampling or water resolve/filter/copy.
Simulation follows the independent existing App controls. Post receives current
merged depth/motion and particle feature motion once; existing SR/Native bypass
and reset contracts are retained. No probes/atmosphere/DoF/FG are enabled.

## Shader work

Identical uniform SPH kernel coefficients are hoisted out of neighbor loops;
float2 density loads are reused; fixed three-axis collision loops are unrolled
to expose scalar component accesses. Sphere intersection shares its repeated
sqrt. Existing arithmetic intrinsics and neighbor traversal/accumulation order
remain, with no approximate-math substitution or solver redesign.
The ordinary FLUID_STATIC_IBL variant removes probe sampling/bindings while the
default shared shader retains its existing probe path.

`BuildOffsets` has a hardware-selected FLUID_WAVE_OPS path: WaveReadLaneAt
exchanges the preceding lane's uint key/index; WaveActiveBallot verifies its
source is active, and the dispatch index must equal i-1 before reuse. Wave calls
precede the count-tail return; first/mismapped/inactive lanes read the original
Sorted predecessor. Thus the contiguous-lane case avoids repeated predecessor
SRV loads without assuming lane order or changing offset values. Devices without
WaveOps compile the scalar fallback. SPH float sums and NarrowRange remain
sequential, preserving their order. This follows the documented
[WaveReadLaneAt active-source contract](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/wavereadlaneat).
No measured speedup is claimed: shader profiling is outside this task.

## Verification and limits

Release x64 build passed. The initial build exposed the Windows `far` macro in a
new local variable; it was renamed to farHDR. The first ordinary frame then
failed command-list Close: the owner treated BufferAccess's UnmanagedState=-1
sentinel as a real resource state. A focused D3D12 debug-message run identified
reserved barrier bits; the owner now checks the sentinel explicitly and preserves
explicit COMMON=0. Temporary executor logging was removed before rebuilding.
The corrected ordinary no-argument launch presents the Bistro/particles/actors
and sharp UI. The status window confirms effective XeSS, render 985×616 and output
1280×800, without fallback. Current-run log has no ERROR/FATAL; large original OBJ
import-budget warnings remain. Saved operations include water simulation/surface,
Far resolve → water optics → Near draw/weighted/resolve → Sorted/additive → post.
In that same launch the user confirmed moving water/composition, after checking
refraction/container occlusion, existing rain and UI: "Да, вода и композиция работают".
The successful smoke is not repeated. Existing fluid,
water composition and Bistro checks are inspected as source context only.
No new tests, solver accuracy comparisons, mode matrix, stress, GPU capture or
profiling is performed. Pause/reset/resize not exercised in that one run remain
source-reviewed. Local probes and App/Meta performance comparison are not started.
Independent correctness, performance, standards, testing and maintainability
source passes found no actionable findings after the sentinel fix. A separate
simplification pass found no necessary changes. The only runtime expansion was
the focused debug-message diagnosis of the failed initial launch, removed afterward.

Artifacts: Build/Verification/meta-bistro-water/.
Executable: Build/bin/x64/Release/App.MetaRenderGraph/App.MetaRenderGraph.exe.
Pre-existing staged SceneManagerModule.cpp, BistroAdditionsTests.cpp, Temp/ and
the old untracked animation task remain outside the task commit. No push.

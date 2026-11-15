# Current MetaRenderGraph frame cost: PIX and source analysis

## Scope

Source HEAD `75c7fda5`. Release x64 build passed before capture. No renderer
source or settings were changed during this investigation. Existing
`BistroAdditionsTests.cpp` and the untracked animation/UI task were untouched.

The current ordinary scene includes SPH water (100,000 particles), animated
NPCs/foxes, GPU particles, grass/wind, shadows, XeGTAO, SSSR, motion blur,
XeSS and UI. The reconstruction overlay showed 985x616 rendering and
1280x800 output. Executable and runtime INI hashes matched before/after;
the manifests are in `Build/frame-audit-2026-11-16/`.

Two warmed-up PIX frames were saved there:

- A: `frame-a.wpix`, also `2026_11_16__1_18_2.wpix`; PIX session `pix-5`.
- B: `2026_11_16__1_18_28.wpix`; PIX session `pix-6`.
- `stages.json`: numerical stage accounting.
- `operations.txt`: ordinary graph operation names for source correlation.

The camera changed between captures. These are two frame investigations,
not a controlled A/B test. PIX 2610.28's bridge is disabled by
`PIX_MCP_BRIDGE_DISABLED`; analysis used pixtool EOP-to-EOP durations and
PS Invocations. Attribution follows work order, expanded draw children and
the corresponding source recording functions. Child draw durations were
not added to parent ExecuteIndirect durations.

## Measured GPU work

Values below are sums of recorded GPU work-event replay counters in the
available PIX context. Clears, barriers, waits and copies are accounted
separately. These values are not ordinary frame time or a reconstructed
cross-queue critical path. In particular the root-event accounting totals
were 260.43 ms and 74.38 ms, substantially above window-title frame times.
Do not convert these sums to application FPS.

| Work | Frame A | Frame B |
| --- | ---: | ---: |
| Water surface depth/thickness/filter/merge | 194.700 ms | 1.803 ms |
| Local shadow mesh draws | 17.285 ms | 18.205 ms |
| Water SPH simulation | 12.574 ms | 12.351 ms |
| Particle simulation/culling/raster | 7.284 ms | 11.681 ms |
| Sun cascade mesh draws | 7.618 ms | 7.541 ms |
| Main mesh depth + GBuffer | 5.225 ms | 7.022 ms |
| Grass build + camera/shadow visibility/draw + wind/interaction | 3.647 ms | 3.787 ms |
| SSSR, denoising, preparation/composition | 1.942 ms | 1.741 ms |
| XeSS reconstruction | 1.616 ms | 1.630 ms |
| Deferred lighting | 0.714 ms | 0.567 ms |
| XeGTAO | 0.441 ms | 0.435 ms |
| Motion preparation/blur | 0.296 ms | 0.617 ms |
| Other mesh transparency/resolve | 0.589 ms | 0.560 ms |
| Water optical resolve | 0.062 ms | 0.081 ms |
| Output/UI | 0.101 ms | 0.111 ms |

Additional root-event counters: barriers 3.98/3.73 ms, waits 0.82/0.83 ms,
copies/query resolution 0.126/0.131 ms. Barrier durations may include the
completion of preceding work and are not proof of avoidable barrier overhead.
Local shadow depth clears alone add 1.17/1.31 ms.

## Water has two distinct costs

### View-dependent raster explosion

`Apps/App/src/Rendering/Water/GDX12FluidPass.cpp:331` and `:335` draw all
particles twice: sphere surface depth and additive thickness.

In A the two events (global IDs 30269/30271) cost 99.613/94.805 ms and each
reports **2,475,884,180 PS Invocations**. In B the corresponding events
(45903/45905) cost 1.050/0.427 ms with **4,214,390 PS Invocations** each.
The large invocation difference establishes a view-dependent raster workload
problem independently of the absolute replay timing distortion.

`Apps/App/Shaders/FluidSurface.hlsl:79` replaces a sphere quad with a full-screen
quad whenever `viewDepth - Radius <= NearPlane`; only spheres entirely behind
the near plane are rejected at `:80`. The branch has no transverse screen-bound
test. `SpherePS` and `ThicknessPS` then call `Intersect`, rejecting irrelevant
pixels only after ray reconstruction, sphere intersection and opaque-depth
sampling (`:84`, `:106`, `:121`). Depth is written through SV_DEPTH.

This is a direct mechanism for pathological overdraw when many spheres lie in
the near-plane depth slab, even if individual spheres are far off-axis.
It is the leading source explanation for A. Exact branch-hit counts and
particle/camera values were not decoded, so causation by that particular
branch remains an inference rather than a measured per-particle assertion.
At 985x616 the A invocation count is about 4,081 full-screen equivalents
per pass; this is not a count of actual full-screen particles.

First investigation target: tight near-plane-aware projected sphere bounds
and rejection before pixel shading, preserving the ray-sphere result and
thickness semantics. Reducing only the NRF filter is unlikely to address A:
the two splat draws account for 194.418 ms of its 194.700 ms surface cost.

### Persistent SPH neighbour work

Both frames contain three substeps. Each records hash/sort, offsets,
density and integration (`GDX12Fluid.cpp:304-363`). Measured sums:

| SPH work | A | B |
| --- | ---: | ---: |
| Density | 4.502 ms | 4.395 ms |
| Integrate | 6.909 ms | 6.784 ms |
| All other SPH work, including save/hash/sort/offsets | 1.163 ms | 1.172 ms |

Thus the first persistent SPH target is neighbour evaluation in density and
integration, not sorting. `PlanSteps` (`GDX12Fluid.cpp:259`) depends on elapsed
time and stability settings; these captures establish three steps here, not
an invariant step count for every camera/run. No stability or quality setting
was changed for the measurement.

## Local shadows are expensive again with dynamic casters

Local mesh work contains 335/385 ExecuteIndirect calls, 25,800/26,008 expanded
draws and 184/207 depth clears. This differs from the earlier static-scene
audit, which had much less local shadow work.

The current source invalidates affected light faces for caster changes
(`SceneLocalShadows.cpp:190`) and grass deformation (`:206`).
`UpdateCasters` compares deformation revisions (`:283`), while PackJob records
needed invalid maps or EveryFrame maps (`:475`). Animated casters and wind-driven
grass therefore cause selected maps to be redrawn, including relevant static
casters in those maps. This is not evidence that the entire cache is globally
discarded every frame: the source uses bounds/face-specific invalidation.

Next target: attribute dirty faces to changing caster owners and evaluate
whether the repeatedly rendered static caster contribution can be reused
without dropping dynamic shadows. The captures alone do not identify the
exact owner responsible for every invalidated map.

Sun geometry remains about 7.5 ms. The outer cascade records 24,066 draws in
both captures and roughly 3.44/3.46 ms. Caster/LOD selection for that cascade
is a separate worthwhile target; blindly reducing shadow quality was not tested.

## Grass and remaining frame work

The ordinary path builds patch geometry once (2.173/2.165 ms). Main-camera
work, HiZ, simulation and wind add 0.179/0.465 ms; sun grass adds
0.335/0.255 ms and local-light grass 0.961/0.902 ms. There are 18/17 local
grass views. Per-view work is now culling indices and drawing, rather than
rebuilding bent vertices. Grass is no longer the leading persistent cost
in these frames.

Particle work is significant (7.3-11.7 ms) and changes with the view. The
recording path separates simulation/death/spawn, visibility/sorting, weighted
alpha and sorted/additive raster (`SceneParticles.cpp`). Further particle
work should distinguish screen coverage from simulation/sort cost rather
than treating all particle work as one dispatch.

AO, lighting, SSR, motion blur and XeSS are measurable but smaller than
local shadows and SPH here. Upload/readback transfers are also not the dominant
recorded cost, consistent with the previous transfer audit.

## Limits

No runtime optimisation was made. CPU scene preparation, job completion,
driver submission time, present pacing, live occupancy and PCIe traffic were
not isolated. The available replay event context and counters do not establish
complete CPU/GPU queue overlap or the application's frame critical path.
The small initial dispatch group was left unclassified in `stages.json`
rather than claimed as complete animation cost. This is a source-grounded
GPU workload ranking, with the water raster anomaly explicitly separated
from the persistent costs seen in both views.

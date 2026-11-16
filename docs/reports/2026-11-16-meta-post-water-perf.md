# MetaRenderGraph: current PIX captures and SPH prediction cache

Captured source HEAD: `5e836132` (includes world canvases, cloud picture and
animated Bistro signs). Both user captures are preserved in
`Build/frame-audit-2026-11-16-post-water/`:

- A: `2026_11_16__22_53_53.wpix`, PIX session `pix-1`, cache `45262cd09142f96c`.
- B: `2026_11_16__22_54_51.wpix`, PIX session `pix-2`, cache `c023c33831860b0a`.

The launch manifest records executable/INI hashes. Captures are different
frames/views, not a controlled A/B. PIX 2610.28 analysis bridge remains disabled
by `PIX_MCP_BRIDGE_DISABLED`; attribution uses pixtool EOP-to-EOP replay counters,
expanded indirect commands and source recording order. These times describe
replayed GPU work, not ordinary application frame latency or a cross-queue
critical path. Parent ExecuteIndirect and its child draws are not added together.

## Current costs

| GPU work | A | B |
| --- | ---: | ---: |
| SPH density + integration, three steps | 11.10 ms | 10.89 ms |
| Sun mesh shadow passes, approximately | 10.96 ms | 7.44 ms |
| Local shadow mesh passes | 6.84 ms | 16.66 ms |
| Local shadow clears | 0.43 ms | 1.42 ms |
| Local shadow maps refreshed | 67 | 222 |
| Grass geometry generation | 2.18 ms | 2.18 ms |
| New unlit opaque water draw | 0.172 ms | 0.173 ms |

SPH kernel IDs are A `41502/41504`, `41584/41586`, `41666/41668` and
B `46310/46312`, `46392/46394`, `46474/46476`. Density costs roughly 1.4-1.5 ms
per step, integration 2.2-2.3 ms. Hash/sort/offsets add work beyond the SPH row.
Water draw IDs are A `103844`, B `101130`. The previous depth/thickness/filter
surface renderer is no longer present. SPH and shadow rendering remain larger
targets than this draw. Shadow totals vary strongly with view/cache invalidation;
the two captures do not identify the changing owner of every dirty shadow face.
`stages.json` retains event IDs and the shadow accounting.

## Retained change

Hash already computes each particle's predicted position and grid cell. Previously
Densities and Integrate recomputed them for every neighbor visit. Hash now stores
both in one per-solver DEFAULT buffer, read by both kernels. Allocation is 24 bytes
per particle (2.29 MiB per 100,000 particles). No per-frame UPLOAD or READBACK buffer
was introduced, and no additional dispatch is required. Every substep refreshes
the cache before its consumers. UAV-to-SRV transitions and graph declarations cover
both local simulation and the WaterTransfer secondary-device path.

Particle count, step plan, time advancement, hash collision rejection, forces and
draw policy are unchanged. The existing sort fixture's manual solver/hash path was
adapted to bind the extra resource; no test cases or mode matrix were added.

## Fixed-input GPU comparison

The isolated benchmark reuses a saved captured-HEAD `FluidSortGpuTests.cpp` fixture
and its original command sequence. Baseline Hash, Densities and Integrate are
compiled from the unmodified captured HEAD shader;
candidate uses the production solver. RTX 3060 Laptop, DIRECT, debug layer off,
100,000 fixed seeded particles, reference-pressure model, three 1/180 s substeps.
Wave acceleration is zero in this fixture. This is a solver microbenchmark, not
the exact Bistro particle distribution, scene FPS, physics debt or CPU timing.

After 120 alternating warmup workloads, three retained A/B pairs each contain five
GPU timestamp samples. Each sample resets the same seed; 0.016667 simulated seconds
and three substeps are identical for both variants. Timestamps include solver work,
barriers and seed copy, excluding CPU preparation and validation readback.

| Pair | Baseline | Candidate |
| --- | ---: | ---: |
| 1 | 8.416 ms | 7.858 ms |
| 2 | 8.358 ms | 7.805 ms |
| 3 | 8.312 ms | 7.743 ms |

Mean: **8.36212 -> 7.80172 ms**, **0.56040 ms / 6.70% less solver GPU time**.
Retained individual samples range 8.28518-8.44493 ms baseline and
7.71584-7.87763 ms candidate.
`prediction-benchmark-original-sequence.log` and
`prediction-summary.json` retain the raw values. The earlier non-warmed runs are
kept separately and are not the quoted result.

Storing intermediate FP32 predictions changes compiler rounding/fusion, so the
first comparison rejected a bit-exact assertion. After three steps, measured max
position difference was 4.78e-7, velocity difference 1.22e-5, relative density
difference 1.27e-6. Formulas are unchanged, but long-term particle trajectories are
not promised bit-identical. The benchmark source/project and baseline shaders are
preserved beside the logs (copy the project back to Apps/App to rebuild it).

## Verification and limits

- Release App.MetaRenderGraph build: PASS.
- Short ordinary smoke: PASS, visible rendered scene, responsive window, no obvious
  failure. XeSS overlay: 985x616 render, 1280x800 output. Smoke process was closed
  before the final isolated comparison; no concurrent scene workload during it.
- Focused GPU timestamp benchmark and the reported three-step numerical comparison:
  PASS at the stated float tolerance; not a long-run fidelity test.
- Extended sort/solver validation suite and secondary-GPU execution: NOT RUN.
- No post-change whole-scene PIX capture or controlled ordinary FPS comparison was
  performed. Do not claim a 6.70% whole-frame speedup.

Next substantial target is shadow geometry: distinguish static redraw cost from
dynamic caster invalidation, preserving point-light coverage and moving shadows.

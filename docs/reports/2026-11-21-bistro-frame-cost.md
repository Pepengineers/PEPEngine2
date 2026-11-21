# Bistro frame cost, 2026-11-21

Release `962d52cc`, RTX 3060 Laptop, stationary camera inside Bistro, TAA/TAAU Balanced, 752×470 render and 1280×800 output. Built in a separate runtime directory after the linker could not open the standard App.exe. The ordinary run showed 24.4 FPS / 41.0 ms and 97% GPU utilization in a spot reading. The interior scene included the moving physics props. This is one view, not a route benchmark.

Reproducible evidence is in `Build/perf-2026-11-21`: `inputs.json`, `interior.wpix`, `interior-counters.csv`, `steady.wpix`, `ordinary-cpu.wpix`, `cpu-summary.json` and `gpu-timeline-summary.json`. The GPU frame and the temporal trace were collected in a PIX-launched process. CPU sampling used a separate ordinary launch, with symbols from the matching Release PDB.

| GPU work | PIX replay, ms |
| --- | ---: |
| Local shadow mesh packet | 19.234 |
| Water SPH | 12.273 |
| Four sun cascades | 10.869 |
| Opaque particles, GBuffer and depth | 2.845 |
| Grass generation, all LOD geometry | 2.164 |
| Main mesh depth + GBuffer | 2.631 |
| Grass draws, 19 extra views | 1.325 |
| SSSR with denoising | 1.063 |
| Cloud picture generation | 0.840 |
| ASTRA canvas / neon canvas | 0.715 / 0.593 |
| Particle weighted-alpha draw | 0.572 |
| Clustered deferred lighting | 0.550 |
| Transparent meshes / WBOIT resolve | 0.342 / 0.024 |
| TAA/TAAU | 0.227 |
| Transparent particle cull and sort | 0.222 |
| Water draw / water pack | 0.173 / 0.024 |
| Motion prep and blur | 0.131 |
| Opaque particle cull | 0.102 |
| Grass depth pyramid, 11 levels | 0.101 |
| Light grid / HUD raster | 0.043 / 0.043 |

Replay values use PIX EOP-to-EOP from top-level markers without counting nested draw/dispatch counters again. They do not add up to the live frame time. Barrier intervals can include waiting for earlier work. The timing trace shows 13,804 DIRECT API executions and 27,464 lists over 72 internal DXGI submissions (approximately 192 executions and 381 lists per DXGI submission in that trace). Over capture time 2–8 s, the union of reported DIRECT/COMPUTE/COPY GPU-work intervals was 2.591 s and the widest gap was 43.2 ms. These are measured under PIX instrumentation and should not be interpreted as ordinary GPU utilization.

Of 2,728 running main-thread samples in the ordinary trace, 1,220 were inside queue submission (45%), 445 elsewhere in the executor (16%), 392 in physics (14%), 159 in local-shadow preparation (6%) and 80 in graph compilation (3%). These are stack samples, not wall-clock time or worker totals. The current executor already skips empty prologue and epilogue lists. `QueueSubmission.cpp:136` still validates command-list device ownership per submission and issues queue execution and a fence signal. Measure the individual CPU API spans before optimizing them. Batching submissions requires explicit resource-state and dependency handling because the graph currently assumes buffer decay at packet boundaries.

## Candidates, in order

1. **Local shadows.** The 19.23 ms packet includes 114.75 million VS invocations. CPU shadow prep also appears in 159 samples. The static and dynamic command lists are already separate. Reuse static depth across dynamic redraws, preserving point-light coverage and PCF inputs. Measure restored-depth copy cost and retained VRAM. Check per-face dirty reasons first.
2. **Opaque mesh particles.** 7.50 million VS invocations against only 1,736 PS invocations in this interior view. Existing cull checks bounds against the frustum and applies a screen-size threshold (`ParticleRender.hlsl:14`). Test conservative occlusion culling against opaque scene HiZ before issuing mesh draws. Keep near-plane and uncertain cases visible. The counters alone cannot distinguish depth rejection from subpixel triangles.
3. **Grass packet consolidation.** Nineteen extra views in the frame cost 0.135 ms in clear dispatches, 0.552 ms in cull dispatches, and 1.325 ms in draws. Their corresponding before-barrier markers total 1.690 ms in replay, including incidental waits. Each clear and cull packet repeats the same binding code and full access list (`SceneGrass.cpp:398`). Put clear and cull in one packet with one bind and the required UAV ordering between them. The clear itself uses the argument UAV, so its standalone declaration can also be narrowed if the split has to stay. Count submissions and measure the affected region after the change.
4. **Grass HiZ.** Eleven dispatches total 0.101 ms and their transition markers total 0.235 ms. Record the mip chain in one packet with UAV ordering between dependent levels. If dispatch cost remains significant, reduce the tail mips in one group. Preserve reverse-Z semantics and conservative handling of odd mip sizes.
5. **Check which canvas layers really animate.** ASTRA and neon total 1.308 ms. The CPU trace also contains 118 inclusive poster samples. Cache an unchanged raster or Gaussian result and update only the animated overlay when its input changes. Determine layer dependencies first. The HUD and final UI composite are already roughly 0.04 and 0.03 ms and are lower priority.
6. **Stop at the cheap passes.** Water pack and WBOIT resolve are each about 0.024 ms. Light grid and AO resolve are both about 0.043 ms. An isolated shader rewrite here is lower priority than removing repeated submissions and binds. Linden is another aggregation candidate (nine views, 0.358 ms work and 0.376 ms before barriers). Inspect its view-specific visibility and ordering before merging anything.

The best small experiment is grass clear+cull consolidation, then the HiZ chain. The best large GPU experiment is reusing static shadow depth. The large CPU path is queue submission. No proposed savings were measured after an implementation, and exterior coverage and camera motion were not profiled in this run.

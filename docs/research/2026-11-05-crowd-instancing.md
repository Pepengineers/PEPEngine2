# Crowd raster instancing — 2026-11-05

Matching skeletal mesh parts now share immutable skinning source buffers and use
hardware-instanced indexed raster draws. Each entity keeps its own animation time
(including the initial random offset), CPU pose, palette and GPU deformation
output. `SV_InstanceID` maps a visible member to its own transform/material and
current/previous deformed vertices. Pause, clip edits and motion history retain
the existing per-entity behavior. This is raster instancing, not a baked animation
atlas; CPU pose evaluation and compute skinning still scale with population.

Visibility is per character and per view before compaction. Groups require the
same mesh handle/generation/content revision, submesh and material. Static draw
commands pass through unchanged. Opaque/transparency, cascaded/local shadows and
reflection capture use the same mapping. `Render.Skinning.Instancing` switches
batching at runtime; shared source buffers remain enabled with the flag off.

## Configuration and method

- Isolated branch `codex/crowd-instancing`, baseline source `9411599`.
- Windows, Release x64, MSVC 14.51; RTX 3060 Laptop GPU, 6 GiB, driver 617.42.
- 1280 x 800; camera position (-300, 200, 20), rotation (0, 117, 0).
- Seed 24301, fixed crowd delta 1/60 s; 120 warmup frames and 600 sampled frames.
- Three runs per configuration, four job workers, three buffered frames,
  skinning High/60 Hz, no SR/frame generation, AO, SSR, reflections, grass,
  water or particles. The original Bistro meshes/lights/shadows remain.
- GPU mean uses exactly 600 completed timestamp pairs, with boundary flushes
  outside/bounding the measured window; it does not average repeated latest
  buffered values. GPU observational percentiles are separately labelled in JSON.
- CPU is process user+kernel time divided by sampled frames, summed across all
  threads. It is neither one-core frame latency nor system CPU utilization.
  Skeletal preparation is wall time for the owner/job preparation section.
- Wall/frame includes normal submission, waiting/presentation and the final
  completion wait. GPU allocation bytes are renderer allocator statistics for the
  complete scene, not the adapter's total resident memory or just character data.
- Fixed delta controls crowd movement/animation. Other wall-clock-driven scene
  effects are not claimed deterministic; optional effects above are disabled.

Run `tools/Measure-Crowd.ps1` after building `Apps/App/CrowdBenchmark.vcxproj`.
The runner saves/restores executable INI bytes and takes the existing cooperative
GPU validation mutex. Other programs that do not take that mutex can still
contaminate timing. It verifies exact startup population and equal crowd checksums
across repetitions. Do not overwrite previous output directories.

## State agreement

All original/instanced runs matched these name-sorted bitwise checksums, including
position/rotation and individual animation time:

| People / foxes | Spawn | After warmup | After sampling |
|---|---|---|---|
| 10 / 5 | 1364975027962268182 | 15445593987108553215 | 2547857015138928335 |
| 100 / 50 | 16992792365034505318 | 1717597905368957147 | 6439355187279064845 |

## Validation

`InstancingTests` checks mixed static/skeletal GPU commands, shuffled visible IDs,
empty lists, buffer growth and nonzero cascade counter offsets. The full Bistro
fixture asserts actual multi-instance commands, preserves the expanded visible
instance count, compares separate/instanced HDR and changes one human/fox clock
while keeping its peer unchanged. The controlled actors/camera sit above the
Bistro to avoid testing characters hidden by buildings.

The first full run had zero relative HDR drift; individual phase edits changed
5162 human pixels and 787 fox pixels. D3D12 debug-layer errors are test failures.
`SecondGroupTests` also exercises source-cache generation/revision ownership,
independent palettes, skinning previous positions and the existing component GPU
integration. The benchmark has a fixed-timer self-check.

## Captures and limits

A four-frame Nsight Graphics 2026.4 baseline capture used base clocks, Ampere GA10x,
Throughput Metrics and per-action timing. The capture warned about dropped periodic
samples, and Release contains no D3DPERF marker ranges; native stage/action exports
therefore contain no stage attribution. The read-only trace decoder supports WRPV
10 and rejected this WRPV 11 capture; native auto-export JSON remains available.
Capture timing at locked/base clocks and profiler overhead is not comparable to
normal benchmark timestamp means. No SM occupancy or bandwidth-bound claim is
made from the incomplete trace.

Original evidence: `Build/CrowdInstancing/original-{10-5,100-50}/` contains per-run
JSON, fixture INI and executable/GPU manifests. Large captures stay under ignored
`Build/CrowdInstancing/nsight-baseline/`; they are not committed. The small raw reports/manifests are preserved in
[the provisional JSON](2026-11-05-crowd-instancing.json).
## Provisional measurements — acceptance paused

The user paused performance work until other agents finish their builds/tests.
Do not treat these numbers as an isolated before/after performance verdict.
Three-run medians from the original and first post-change sessions are retained
for continuity; side work contaminated the post-change sessions:

| Session | GPU ms/frame | Aggregate CPU ms/frame | Wall ms/frame | GPU allocation MiB | Allocation count |
|---|---:|---:|---:|---:|---:|
| original-10-5 | 17.966 | 7.031 | 18.007 | 3052.68 | 1006 |
| original-100-50 | 29.568 | 17.500 | 30.098 | 3466.36 | 5461 |
| instanced-10-5 | 20.350 | 10.104 | 20.418 | 3054.93 | 947 |
| instanced-100-50 | 37.163 | 33.568 | 53.781 | 3311.11 | 3917 |

For 100/50, the same-binary Off control produced GPU means 31.094, 31.720 and
31.878 ms; On produced 34.619, 36.235 and 37.242 ms. External compiler/linker
processes were observed during On repetitions 2/3, and the attempted small-crowd
control was stopped before starting when a build was detected. The earlier
post-change dense session also overlapped BistroPhysicsTests and linking.
These observations justify withholding a speedup claim and keeping batching
opt-in; they do not isolate a universal regression percentage. Laptop boost,
thermal limits and shared CPU/GPU power budgets remain uncontrolled.

Memory statistics are stable across repetitions: 100/50 uses 155.25 MiB fewer
allocated GPU bytes and 1544 fewer allocations with batching On than the original.
At 10/5, scratch buffers add 2.25 MiB while allocation count falls by 59. These
are whole-renderer changes from shared skin inputs, initial reservations and
batch scratch together; they are not the draw-batching-only memory effect.
The same-binary Off dense control uses 3829 allocations and 3293.24 MiB.

The final source differs from the controlled executable only in the batching
CVar default (`false`), runner default (`On`) and documentation. Both measured
modes set the CVar explicitly; their algorithms are unchanged. The final benchmark
binary was rebuilt and its fixed-timer self-check passed after the default edit.

Resume with fresh output directories, repeat Off/On at 10/5 and 100/50 while no
other builds/benchmarks run, and keep the exact 600-timestamp/checksum assertions.
The original renderer baseline, executable and shader snapshot are preserved
under `Build/CrowdInstancing/baseline-snapshot`; that old executable embeds this
worktree's shader path, so running it against the current shader files is invalid.
No post-change Nsight capture was taken after the user requested the pause.

Full Release builds and GPU/CPU tests passed: InstancingTests, clean-INI
SecondGroupTests, BistroAdditionsTests and its routes-only mode. The full A/B
fixture preserved all 12 visible instances while reducing opaque commands from
12 to 6, with zero HDR drift. SecondGroupTests initially reused an autosaved INI
with fluid disabled; its repeat passed after restoring the committed fixture.
Tests must start from a known INI because ConsoleModule persists runtime edits.

Review found no remaining confirmed defects after correcting the benchmark INI
backup/restore lock order. Coverage limits remain: no numerical raster velocity
A/B for a moving/settling crowd, and no dedicated batched/separate readback of
animated transparent/probe actors. Existing previous-position compute and full
scene/probe compatibility tests passed. Other GPUs and large-population soak
runs have not been checked.
## Integration into Bistro rendering — 2026-11-07

Merged `codex/crowd-instancing` into `codex/bistro-rendering`, preserving the newer
application-owned pipeline, geometry-view batching, physics and resource caching.
Skeletal vertex/remap root SRVs now travel through `GDX12GraphicsBindings`; view
passes submit compacted mesh commands through `EmitGeometry` and retain their
existing application-source batches. Both initial-crowd reservation and the newer
per-attachment reservation remain active, including free-instance reuse.

Release App and InstancingTests builds passed. RenderPipelineTests passed its
primitive/batch, pipeline composition, AO rebinding and resource-lifetime checks.
Full InstancingTests passed: 12 visible instances, 12 separate commands -> 6
instanced commands, zero HDR drift; individual human/fox phase edits changed 5400
and 792 pixels respectively, with unchanged peer clocks and no D3D12 debug errors.

Performance measurement remains paused at the user's request. Earlier numbers
belong to the pre-integration renderer and must not be presented as a comparison
with the current Bistro/physics pipeline. Resume using the integrated binary's
explicit Off/On modes (or an equally instrumented integration-parent baseline),
with frozen inputs and no concurrent performance work.
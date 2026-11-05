# Application render pipelines: validation record

Implementation checkout: `D:/Project/PEPEngine2/.worktrees/bistro-render-extensions`,
branch `codex/bistro-render-extensions`. This record describes that isolated
checkout; it is not evidence of integration into the main working directory.
Bistro now composes its own graph, the default pipeline runs without app-effect
registrations, and the shared runtime executes both. CPU/GPU checks and targeted
Debug/Release builds passed, including the final 36-frame SR/FG smoke with clean
D3D12 validation. [Final acceptance](#final-acceptance) records the tested scope,
corrected failures and remaining hardware/coverage limits.

The stage records below retain their checkpoint results. Final acceptance
supersedes earlier pending items and visual measurements.

## Baseline and provenance

- Original code base: `9411599030eac89940c49bb201d789631f24103c`.
- The interrupted implementation's verified grass relocation was preserved in
  `c3f6153`. Its extension interface is superseded by the current plan.
- Approved architecture, plan and terminology from documentation-only `e4780d2`
  were cherry-picked as `53cbcb1`.
- `6dea962` integrates the existing async/particle implementation from a precise
  snapshot of 23 relevant files in the main checkout at HEAD `e4780d2` with render
  WIP. It preserves queue/fence APIs, physical buffer accesses, particle controls,
  timer behavior, resource-state fixes and their tests. Physics WIP was excluded.
- Exact source bytes, prior worktree bytes, merge-base bytes and SHA-256 manifest
  are retained in `Build/Verification/main-render-snapshot/`. Three-way merging
  normalized line endings; no semantic merge conflicts remained.
- A proposed merge of the newer committed code base was not completed after
  automatic review rejected its unrelated physics scope. That local merge was
  aborted before the documentation-only cherry-pick. No main-checkout files,
  index, commits or conflicts were changed.

The main checkout still has independent uncommitted physics/render/particle
work. Future integration must reconcile its then-current changes with this
recorded snapshot. Tests below do not validate that evolving working tree.

## Checks before the new pipeline extraction

MSBuild: Visual Studio 18 Professional, v145, Debug x64. Build outputs and pinned
dependency builds remain inside this worktree. MSVC build helpers require
execution outside the filesystem sandbox; the initial sandbox attempt reported
`C1902`. The pinned inih/rapidyaml submodules were initialized locally.

| Check | Observed result |
| --- | --- |
| Debug RenderGraphTests build | Passed |
| Debug SecondGroupTests build | Passed |
| SecondGroupTests GPU integration | Passed, clean D3D12 debug layer |
| Particle direct/async/direct and queued slots | Passed within SecondGroupTests |
| RenderGraphTests CPU scheduling/recording/pooling | Passed |
| RenderGraphTests async buffer publication and texture handoff assertions | Passed |
| RenderGraphTests final debug-layer scan | Failed: a non-simultaneous-access texture still referenced on DIRECT was read on COMPUTE |
| Six-key output INI migration | Passed: common settings retained, original backed up, repeat execution safe, existing backup protected |
| Relocated grass/wind shaders | 15 unique DXC SM6.6 entrypoint/define variants passed, including capture VS/PS and all six wind stages; runtime variants also exercised by SecondGroupTests |

The graph validation failure was observed **before** extracting the new
pipeline, on the imported implementation. It is a known limitation of this
baseline, not a clean async validation result. No test was weakened and no
message was suppressed. Logs are under `Build/Verification/baseline-*.log`.

Earlier checks before importing that async snapshot passed the then-existing
RenderGraphTests and SecondGroupTests with a clean debug layer. They apply to a
different configuration and cannot establish that the current async graph is
clean.

## Implementation acceptance

Pipeline extraction is preserved in `c8cb624`. Default and Bistro own explicit
composition; the runtime retains execution and synchronization. The old
extension/stage/tail-provider API is removed. A temporary geometry-only source
is still being replaced by primitive/batch publication.

| Pipeline extraction check | Observed result |
| --- | --- |
| Debug RenderPipelineTests, BaseRenderPipelineTests, App, SecondGroupTests | Builds passed |
| CPU composition and AO early/late/early rebinding | Passed; repeated rebuilds preserve pass count and producer/consumer links |
| Independent application pipeline GPU | Passed: one backbuffer pass, no common-technique helper, three frames, resize, clean validation, absent SR/FG queries rejected |
| Default pipeline empty-scene GPU | Three frames, resize, pass count, common transparency/particle nodes and D3D12 validation passed |
| Base-only registration assertion | Deferred failure at `Render.Fluid.Enabled` until the water relocation removes its current Engine registration |
| SecondGroupTests GPU after extraction | Passed, clean D3D12 validation, including direct/async/direct transitions |
| RenderGraphTests GPU after extraction | Passed, including async texture handoff and a clean final infoqueue scan; graph executor unchanged |

The first empty-scene smoke exposed a zero-template indirect-draw access
violation. Its stack was captured before the fix. Culling/opaque/WBOIT now skip
only zero-sized mesh stream operations and retain clears and other geometry;
the same smoke then completed. Temporary diagnostic instrumentation was
removed. Logs use `Build/Verification/u1-*-build.log` and `u1-*-run.log`.

The earlier async texture error did not reproduce in this later graph run. It
remains an observed intermittent baseline failure, not a confirmed fix or a
long-run stability result.

Shared mesh primitive/batch publication and emission are preserved in
`e8f1028`; the prepared-view phase contract and CPU selection/split checks are
in `06e5cd7`.

| Shared mesh batch check | Observed result |
| --- | --- |
| Debug App.Base, App, RenderPipelineTests, SecondGroupTests, ReflectionProbeTests | Builds passed |
| CPU registry/source/draw contracts | Passed: generation/reuse, snapshot pruning, revisions, filters, external direct/indirect batches, offsets/strides, formats and physical-read declarations |
| Empty source batch | Recorded failure before the fix, then passed without touching null resources |
| SecondGroupTests GPU | Passed, clean validation; skinning, material/light edits, previous motion, queued slots and particle queue transitions retained |
| ReflectionProbeTests --debug GPU | Passed, clean validation; opaque variants, WBOIT, probe shadows/faces/cache/frozen modes, queued publication, resize and two-probe budgets |
| Prepared-view CPU phases | Passed: inactive slots skipped, split head does not draw or finalize, completion runs after selected view jobs |

An incremental MSVC relink reported an invalid COMDAT. A scoped App.Base
object rebuild and nonincremental App link recovered; dependency/source cleanup
was not required. Build and runtime logs use `Build/Verification/u2a-*` and
`u2b-view-contract-*`.

The mesh checks still used the temporary grass draw bridge. Grass's explicit
preparation/batch conversion, water ownership and final build/routing/lifetime
acceptance remain to be recorded. The architecture documents do not claim that
the full implementation is complete.

### Prepared grass, shadow and probe views

The U2b implementation, checkpointed in `962404c`, replaces the temporary draw bridge with CPU
snapshots, explicit roots/wind and per-view expansion nodes, and the shared
geometry batch emitter. Static-only views exclude dynamic grass. Shadow/probe
cache selection occurs before graph execution; finalization publishes technique
outputs only after the application ranges finish. AO binding now precedes view
preparation, including a same-frame early/late switch.

| Prepared geometry check | Observed result |
| --- | --- |
| Debug RendererDX12, App.Base, App, RenderPipelineTests, SecondGroupTests, ReflectionProbeTests | Builds passed |
| CPU pipeline/prepared-view/alias ordering tests | Passed |
| Final SecondGroupTests GPU | Passed, clean D3D12 validation, including same-key static-filter exclusion |
| Two-view queued grass graph | Passed across all three frame slots; visible/empty view readbacks remain distinct; stale overwritten range rejected |
| Frozen depth/GBuffer expansion | Single-blade vertex bytes match exactly |
| Grass primitives | Stable identity, wind-time invalidation of shadow-only geometry, removal passed |
| Final ReflectionProbeTests --debug | Passed, including repeated Prepare, cache/publication, frozen/baked/dynamic modes, off-camera caster, queued slots and resize |

The focused two-view graph has 11 nodes and two indirect submissions per frame.
Actual geometry arena allocation was 1,179,648 bytes for either one or sixteen
default 1,232-blade patches, and 515,964,928 bytes for sixteen maximum-valid
262,144-blade/eight-segment crossed-ribbon patches. The arena is shared across
direct-queue views and frame slots. These numbers cover vertices, indices and
arguments; they exclude roots, wind, constants and retired growth allocations.
They are allocation measurements, not an FPS result or total memory budget.

The first repeated probe run loaded its previous disabled output INI; the GPU
fixture now explicitly enables reflections. After that correction, D3D12
validation caught a new shadow-map SRV bound before its depth-write transition.
Moving target/state restoration before descriptor binding fixed that failure;
the full regression then passed. Temporary diagnostics were removed. Evidence:
`Build/Verification/u2b-second-final-*`, `u2b-probe-barrier-*` and
`u2b-pipeline-*`.

Full Bistro graph acceptance, water relocation and final Debug/Release and
hardware compatibility checks remain outstanding at this checkpoint.

The first full Bistro scene run then exposed an access violation in static
primitive extraction. A temporary symbolized stack identified reads from
`InstanceCache`/`TransformCache` CPU mirrors that had not been enabled. Enabling
their existing change-tracked mirrors before resize/upload fixed the first-frame
crash, including the same reads in skeletal publication. This fix is `8c17a23`.
Logs: `u2b-bistrograph-diagnostic-*` and `u2b-bistrograph-mirrors-*`.

The next scene run reached dedicated HDR readback but failed a later byte
comparison. A second dedicated run reproduced the difference before pooling:
296 half-components in 172 pixels, within x=709..809/y=497..522. The differing
area contains particles. Atomic alive/index compaction still runs at zero
simulation delta, changing the order of FP16 particle blending. Disabling only
particle drawing made repeated dedicated and pooled outputs bit-identical.
The final fixture keeps exact byte comparison, uses the Bistro entrance camera,
and restores particle drawing for its resize/upscaler smoke. It does not use a
pixel tolerance. Temporary stack/raw-image diagnostics were removed from test
sources; their evidence remains ignored under `Build/Verification`.

## App-owned water and content acceptance

Water ownership and explicit composition are checkpointed in `fbd45e2`.
Engine/App.Base no longer contain fluid extraction, storage, component pools,
shader paths or the fluid shared graph token. The existing Bistro light-fixture
bootstrap remains in App.Base; moving lights was explicitly deferred by the
approved plan. The audit therefore does not claim that every occurrence of the
word Bistro disappeared from shared code.

| Water/ownership check | Observed result |
| --- | --- |
| Debug RenderPipelineTests | Build and CPU checks passed: optional links, missing-producer rejection, rebuild, pause/time scale, two worlds, immutable seeds, entity reuse and listener cleanup |
| Base-only default GPU | Passed: frames, resize, clean validation, no water/grass/wind CVars linked; old output INI migrated with the scoped backup tool |
| Independent application GPU | Passed: one clear pass, resize, absent reconstruction/FG capability rejection |
| SecondGroupTests GPU | Passed, clean validation: first-frame physical buffers, SPH/NRF/resolve, depth/motion/HDR aliases, quality 0/1/2/3, simulation-only, clear/reset/off |
| Particle composition oracle | Exact HDR equality against ordinary transparency for three single-particle blend modes across absent, empty-seed and disabled water |
| Aerial perspective oracle | Exact HDR equality against the single-aerial reference for empty/disabled water, both an alias branch and a forced final-composition branch; atmosphere has measurable effect |
| Real BistroApp content GPU | Passed: default/zero/changed counts, world bounds, probes, walking/pause, visible grass/water/human/fox pixels and clean D3D12 validation |
| Bistro graph HDR | Exact dedicated/dedicated/pooled/dedicated equality passed for both AO positions; odd resize and native/XeSS switch passed with particles restored afterward |
| Relocated fluid and neutral shaders | All 19 DXC SM6.6 variants passed; runtime SM6.8 variants also exercised by GPU tests |
| Release x64 App | Build passed |

The first content checkpoint measured 43,628 changed grass HDR pixels, 15,766
water, 24,109 human and 15,687 fox pixels. Those pre-`ac83fcb` measurements are
superseded: static identity transforms were not all uploaded correctly. Final
measurements below include the restored static scene. Images are in
`Build/BistroAdditions/`. Its final compiled graph has 4,620 pass nodes, including
fixed view/range jobs that remain present while cache-hit/inactive views do no
geometry work. This is a measured topology cost, not a performance improvement;
no baseline/candidate FPS comparison was made. Saved graph:
`Build/Verification/bistro-with-content.dot`.

Evidence: `u3-pipeline-final-*`, `u3-base-*`, `u3-custom-*`, `u3-second-*`,
`u3-additions-*`, `u3-bistrograph-particles-off-*`,
`u3-bistrograph-camera-*`, `u4-water-shaders.log` and
`u4-app-release-build.log` under `Build/Verification`.

The active-water transparent-mesh partition does not have a new isolated pixel
oracle in this task. Its shader comparisons are preserved while terminology and
ownership change; real water pixels and the off/empty composition cases above
are verified. Final compatibility/review results and hardware limits follow.

## Final acceptance

### Upload initialization and visible scene

`ac83fcb` fixes the generic change-tracked upload mirror, rather than adding a
scene-specific workaround. Its old CPU mirror used `T{}` while the mapped GPU
allocation contained zero bytes; a default identity transform could therefore
appear unchanged and skip its first upload. `TrackChanges` now snapshots mapped
bytes with the actual element stride, including after growth. The new
`MemoryAllocatorTests` regression failed before the fix and passed afterward:
packed/aligned buffers, nonzero defaults, growth, enabling tracking after an
upload and unchanged-write revisions are covered. Evidence:
`u4-mirror-red-run.log`, `u4-mirror-green-run.log`.

The post-fix real `BistroApp` test passed with clean D3D12 validation. Fixed
comparisons measured **57,090 grass**, **15,766 water**, **99,701 human** and
**149,600 fox** changed HDR pixels. Probe previews covered 277,487 sphere pixels;
turning previews off left zero RGB drift. Static Bistro buildings are visible
in the final captures, which were inspected after this fix. The graph still
contains 4,620 compiled nodes. Evidence: `final-additions-fresh-run.log`,
`Build/BistroAdditions/*.ppm`, `water-proof.png`, `grass-proof.png`.

That test's old output INI contained dynamic `App.Bounds.*` keys registered only
after strict startup INI loading. This pre-existing startup-order issue was
confirmed in the original baseline. The acceptance run used a fresh isolated
executable/DLL/config directory under `Build/Verification/runs/post-mirror-bistro`;
the original output INI was preserved. Unknown-key validation was not relaxed.
Reusing such an old dynamic-bounds INI remains an existing limitation.

### Concurrent texture reads

The baseline async texture validation failure reproduced during final checks.
`d2d2038` adds a COMMON handoff before the explicit read state at a compute fork,
matching the executor's buffer handoff. This permits concurrent read ownership
after prior writes, as described by the
[D3D12 resource heap specification](https://microsoft.github.io/DirectX-Specs/d3d/ResourceHeaps.html).
It does not serialize the read-only branches or add a scheduler.

Three complete `RenderGraphTests` runs then passed with a clean infoqueue.
Existing assertions were unchanged: concurrent-reader join remains at frame
retirement, exact texture readbacks and 40 queued frames remain checked.
`SecondGroupTests` also passed its direct/async/direct particle sequence after
relinking this executor. Evidence: `final-async-common-run-{1,2,3}.log`,
`accept-second-run.log`. Earlier intermittent passes above were not treated as
proof of the fix.

### Deterministic HDR comparison

Restoring the static meshes exposed a second source of nondeterminism in the
full-scene byte comparison. Two dedicated captures differed in 212 pixels,
within x=590..852/y=333..426, over the interior bottles. `Culling.hlsl` compacts
transparent indirect draws with `InterlockedAdd`; changing draw order changes
FP16 WBOIT rounding. Setting only transparent material opacity to zero made
dedicated/dedicated/pooled/dedicated captures exactly equal at both AO placements.
This is separate from the particle-compaction issue recorded above.

The fixture preserves exact comparison and adds this deterministic material
setup. It restores every original opacity and particle drawing before the
odd-resize/native-to-XeSS smoke. Thus byte identity covers the static opaque
scene and graph routing with zero transparent contribution; it does **not** claim
bit-identical nondeterministic transparent blending. Nonzero WBOIT, particles
and off/empty water composition have separate GPU pixel oracles. No tolerance,
validation suppression or production culling change was introduced. An attempted
`StaticMeshRenderComponent::Visible` control did not affect the legacy GPU draw
stream and was removed from the fixture. Evidence:
`diagnostic-hdr-run.log`, `diagnostic-hdr-opacity-control-run.log`, ignored raw
readbacks and `hdr-{reference,current,diff}.png`.

Both AO positions retained 1,560 graph nodes in this fixture: dedicated mode
used 16/16 physical/transient textures and pooling used 10/16. Saved graphs:
`Build/Verification/bistro-{early,late}-ao.dot`,
`Build/Verification/bistro-with-content.dot`,
`Build/Verification/default-render-pipeline.dot`, and the independent
one-pass `Build/Verification/application-clear.dot`.

### Final compatibility checks

The 36-frame SR/FG smoke first failed the Intel generated-frame assertion.
Diagnostics showed two independent continuous-history resets: wall-clock
Debug/SDK stalls exceeded the existing 250 ms pause threshold, and the fixture
changed AO placement every frame, rebuilding the graph and requesting another
reset. Both runtime behaviors exist in the selected baseline. The fixture now
stops its wall-clock timer (its movement jobs already receive explicit dt) and
changes AO placement in six-frame phases. Camera cuts, restart, both placements,
all SR providers, AMD/Intel FG and every generated-frame/fallback assertion remain.

With consecutive history available, Intel generated six frames by frame 35, but
the final infoqueue check exposed a real SDK state mismatch. Ordinary resource
helpers preserve read-only DSV bits; FFX/FG input descriptors instead declared
exact compute or pixel/compute read. At those two SDK boundaries, a depth
resource retaining `DEPTH_READ` now transitions through COMMON before the
existing declared read transition. General read-only DSV behavior is unchanged.
The same complete smoke then passed with **clean D3D12 validation**, real AMD
and Intel generation and all 346 moving props. Evidence:
`finish-sr-fg-run.log`, `diagnostic-fg-run.log`,
`diagnostic-fg-fixed-clock-run.log`, `diagnostic-fg-stable-history-run.log`,
`diagnostic-sdk-depth-run.log`. Temporary diagnostic output was removed.

All paths below are relative to `Build/Verification/`. Builds use VS18/v145 x64
and nonincremental linking. A HostX86 linker heap/PDB failure recovered with
`PreferredToolArchitecture=x64`; this changed the build host, not the source.
Third-party DirectXTex missing-PDB warnings remain.

| Check | Observed result |
| --- | --- |
| Post-mirror Debug builds | All 13 targeted projects passed: App, BistroAdditions, SecondGroup, BistroGraph, BasePipeline, RenderPipeline, LightingGpu, ReflectionProbe, RenderGraph, ObjectDataJobs, SSRBistro, Loading, Startup |
| Final Release builds | App, App.Editor, RenderPipelineTests, SecondGroupTests and BaseRenderPipelineTests passed after COMMON handoff and helper reuse |
| Final Debug consumers | SecondGroup, BistroGraph, ObjectDataJobs, App and App.Editor rebuilt successfully after helper reuse; BistroDofSmoke built in Debug and Release |
| Release allocator and pipeline | Builds and runtime passed, including nonzero-default CPU mirrors and pipeline ownership/composition contracts |
| Final Debug/Release SecondGroup GPU | Both passed clean, including grass multi-view arena, water aliases, particle async switching and exact composition oracles |
| Final BistroGraph | Passed: exact deterministic HDR at both AO positions, 16/16 vs 10/16 textures, restored transparency/particles for resize and native/XeSS |
| Release Base-only GPU | Passed: default frames, resize, no app-effect registrations and clean validation |
| RenderPipelineTests CPU | Passed: pipeline composition, external geometry source, prepared views, ownership/lifetime, water optional links and world snapshots |
| LoadingTests | Passed: native meshes/cache, WIC/BC7/PNG, CVars, priority and unload |
| LightingGpuTests | Passed clean: point/spot shadow cache, deferred/OIT local shadows and frustum light grid against brute force through 769 lights |
| ReflectionProbeTests --debug | Passed clean: static/dynamic/frozen/baked cache, off-camera casters, queued publication, resize and probe budgets |
| Current default BaseRenderPipelineTests GPU | Passed clean: empty frames, resize, common particles, no app-specific CVars |
| Current independent BaseRenderPipelineTests --custom | Passed clean: one application pass, resize, absent reconstruction/FG capability checks |
| ObjectDataJobsTests 1/4 threads | Passed: actual indirect slots, all frame-slot bounds, immutable source templates |
| ObjectDataJobsTests --render-bistro | Debug and final Release passed: 36 frames, every SR provider, independent AMD/Intel FG with generated frames, camera motion/cut, restart, quality changes, odd resize and clean validation |
| Final SDK Release builds | ObjectDataJobsTests, App, App.Editor, SecondGroupTests and BaseRenderPipelineTests passed with the exact SDK depth-state handoff; clean-source Debug ObjectDataJobsTests also rebuilt |
| SSRBistroTests | Passed clean: 304 existing bottles, moving props, quality/off/re-enable, odd resize and HDR XeSS/output |
| BistroStartupTests | Passed: both scene parts, 346 props, stable camera, background/focus/minimize/resize and automatic settings UI/input capture |
| Configuration/shaders | Six-key backup migration checks, 15 grass/wind and 19 fluid/neutral shader variants passed; strict validation retained |

Compatibility logs: `final-*-run.log`, `accept-*-run.log`,
`accept-*-debug-x64host-build.log`, `accept-*-release-build.log`,
`finish-*-debug-build.log`, `sdk-final-*-build.log` and
`sdk-final-sr-fg-release-run.log`.

The source/project audit found no concrete water/grass/wind types, moved shader
paths or obsolete extension/provider tokens in Engine/App.Base. This audit
supplements the base-only link and GPU checks. No project-wide standalone lint
or typecheck command is configured for this C++ solution; MSVC compilation,
`git diff --check`, shader compilation and the listed tests are the checks used.

### Completed review

`ce-code-review mode:agent base:6dea962` completed against the tracked working
tree and the approved plan. Receipt: **status complete**, **Ready to merge**,
zero primary/actionable findings, run `20261105-034754-8aecfda5`. Eight independent
local lenses covered correctness, standards, testing, maintainability,
performance, API contract, reliability and adversarial review. The late SDK
state and fixture changes received focused follow-up before completion.

The read-only reviewers did not rerun the parent's native/GPU checks. No
different-provider CLI was installed; a local adversarial reviewer covered
that lens. No unresolved finding prevents the approved implementation; the
coverage limits below remain explicit. The review verdict applies to this
isolated worktree, not a merge into main.

The compact [review receipt](../reviews/ce-code-review/20261105-034754-8aecfda5/review.json),
metadata and stage record are versioned. Full generated diff/dispatch artifacts
remain local in that run directory and are ignored by its own `.gitignore`.
Final production compatibility fixes are checkpointed in `b596a41`; the final
consumer/fixture acceptance is `402848c`.

### Limits

- Hardware checked: NVIDIA GeForce RTX 3060 Laptop GPU, WaveOps enabled. Only
  the primary device was exercised. Secondary-device initialization was already
  disabled in the selected baseline; source routing is retained, hardware
  execution is unverified.
- No long soak, FPS comparison, graph-overhead profile, new node editor, hot
  reload, async scheduler or multi-GPU expansion was performed. The 4,620-node
  topology and grass arena allocations above are costs, not speedup evidence.
- The active-water transparent-mesh split lacks a new isolated pixel oracle;
  existing shader partition comparisons, real water pixels and the separate
  off/empty particle/aerial cases were checked.
- This branch contains the recorded render snapshot, not the evolving main
  checkout or its physics changes. Nothing was pushed or integrated into main.

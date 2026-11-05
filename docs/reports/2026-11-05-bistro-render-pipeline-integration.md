# Bistro render pipeline integration

Target working branch: `codex/bistro-rendering`, starting at `d360368`.
Source: `codex/bistro-render-extensions` at `a1458c8`.
Common ancestor: `9411599030eac89940c49bb201d789631f24103c`.
The previous pipeline acceptance record describes an isolated checkout; this
record concerns the actual combined working branch.

## Preserved work

Before the merge, tracked files and the index were clean. The current physics,
render and particle work was already committed. The local backup ref
`codex/bistro-before-pipeline-integration-20261105` retains the complete original
HEAD. Existing untracked `.tmp/` and `.worktrees/` were excluded and preserved.

Of 174 paths changed on the working branch since the common ancestor, 35 also
changed on the pipeline branch. All other **139 main-only paths remain unchanged**,
including Jolt integration, fixed-step/world lifecycle, seeded prop waves,
physics scenes/assets and their existing tests. Overlapping application paths
retain physics-before-render-preparation ordering and physical prop creation.

## Integration decisions

- The newer working-branch command queues, resource state/concurrent-read
  ownership, graph executor and 400-changing-frame graph regression are retained.
  The old borrowed snapshot's retirement groups and external-resource API are
  not reintroduced. `InvalidateGeometryBuffer` supports the shared batch emitter;
  the concrete fluid shared token is removed.
- World retains rigid-body/collider pools. Water/grass/wind pools remain owned
  and explicitly registered by the application. Both pipeline constructors and
  the shared runtime coexist with the current PhysicsModule.
- GBuffer retains the current explicit read-only depth declaration and DSV,
  together with the new prepared geometry/batch consumer. Local-shadow uploads
  remain in common frame preparation rather than running twice.
- Bistro fixtures retain current direct/async switching, active AO algorithms,
  graph timing entrypoint, resize, physics and SR/FG paths. The expected first
  queue join is now `Fluid surface`: its Dynamic depth output can forward to
  opaque depth, so publication is a declared alias hazard. This is a conservative
  join required by the new graph declarations, not a scheduler rewrite.

## Issues found by repeated checks

The probe test's descriptor budget omitted the read-only GBuffer DSV that the
working branch now creates during initialization. Its exact assertion now
expects seven SRVs, six RTVs and two DSVs: writable prepass depth plus its
read-only GBuffer view. It still guards against allocating skipped textures.

An intermittent exact HDR comparison failed on a DIRECT/DIRECT comparison:
nine pixels, x=764..772/y=473..474, maximum RGB delta 1.861328125. This was not
specific to async compute. Rays through all nine pixels against the actual
native interior mesh cache identified exactly coplanar triangles on
`Paris_Plate_01B__3_`: submesh 2090 `Plates_Ceramic` and submesh 2091
`Plates_Details`, with different material/UV data. The latter is the masked
overlay from `Interior/interior.mtl`. Atomic opaque command compaction and
EQUAL-depth GBuffer rendering do not impose deterministic precedence between
these authored surfaces.

For byte identity only, the fixture now excludes that overlay along with the
already excluded transparent/particle contribution, and restores material type,
opacity and particle drawing before ordinary resize/SR rendering. No pixel
tolerance, asset edit, production draw-order change or validation suppression
was added. The raw readbacks and lossless views are retained under the
verification directory. The asset's production coplanar ordering ambiguity
remains an existing limitation.

## Verification

All build/run logs are under `Build/Verification/bistro-pipeline-integration/`.
Fresh `bin/{Debug,Release}` and intermediate directories use `validation.props`,
so previous build outputs and user output INIs are preserved. Runtime variants
requiring a fresh application config use separate `runs/` directories. Builds
use VS18/v145 x64 and the x64 host linker. Existing third-party missing-PDB
warnings are retained.

Initial build/check results:

- 18 targeted Debug projects built, including App, Editor, physics, pipeline,
  graph, particles/water/grass, probes, lighting, loading and Bistro consumers.
- Eleven Release projects built: App, Editor, PhysicsTests, BistroPhysicsTests,
  RenderPipelineTests, SecondGroupTests, BaseRenderPipelineTests,
  ObjectDataJobsTests, MemoryAllocatorTests, BistroRenderGraphTests and
  BistroAdditionsTests.
- Debug RenderGraphTests passed: 400 changing frames with both concurrent
  readbacks, writer/unmanaged joins, queued allocators, failure cleanup and a
  clean D3D12 debug layer.
- PhysicsTests passed in Debug and Release: 1000 colliding bodies, 180 ticks,
  one/four shared job threads, world isolation, pause/time scale and fixed phases.
- Bistro wave unit tests and RenderPipelineTests passed in Debug and Release.
- Debug SecondGroupTests passed its grass multi-view arena, water aliases,
  particle direct/async/direct and exact single-composition checks cleanly.
- Full Debug Bistro graph matrix passed: exact HDR, direct/async switching,
  pooling, both AO placements/all AO algorithms, resize and native/XeSS. The
  plate-overlay control receives final repeated checks below.

Additional combined-branch checks passed in Debug: default and custom base
pipelines, allocator, lighting, SSR Bistro, full Bistro startup, object-data jobs,
36 SR/FG frames, and loading. The actual Interior prop partition passed with
346 props, 954 prop submeshes and 1427 static submeshes, with exact coverage.
The initial no-argument partition invocation was invalid; the successful run
supplied the artifact directory and actual Interior OBJ as required.

The startup test completed naturally in approximately fourteen minutes. A late
native stack sample located active time in Jolt collision work during frame
simulation, after initialization. This sample does not attribute the whole
duration or establish a merge performance regression.

Debug additions wave/content runs were stopped only by exact owned executable
identity checks after partial progress. They are incomplete, not passes. Full
unchanged assertions are repeated in Release, whose additions harness explicitly
enables the D3D12 Debug Layer and checks its error queue independently of `_DEBUG`.

The final plate-controlled Bistro graph matrix passed in **both Debug and
Release**: 32 HDR captures per configuration with exact comparisons,
DIRECT/DIRECT/async/DIRECT
switching nested in dedicated/dedicated/pooled/dedicated allocation, both AO
placements, all eight active-AO combinations, odd resize and Native/XeSS.
Corrected ReflectionProbeTests `--debug` passed its full depth/radiance/cache,
publication, overlap, parallax, roughness, shadows and runtime-switch suite.
The config migration test also passed, including backup/idempotence guards.

The complete Release physics scenario with 758 bodies, one and four job threads
and three waves passed in 1827 ticks each: minimum launch 500 cm/s, minimum scatter
700 cm/s and peak speed 3000 cm/s. It reproduced the existing 16 escapes and
16 static-chord candidates; these diagnostics do not fail its functional scope.

The Release real-Bistro GPU wave scenario passed all three lift/scatter/fall
cycles, stable body counts, finite HDR and the explicit D3D12 error-queue check.

The Release ordinary additions run also passed: default/zero/changed counts,
walking skeletal animation, pause, bounds, grass/water visible GPU contribution,
NPC/fox visibility and a clean D3D12 debug layer. NPC and fox changed 35,248 and
144,009 HDR pixels respectively; the complete content graph had 4,620 passes.
All five entries in `final-runtime-results.json` have exit code zero.

Integration code review completed with status `complete`, verdict **Ready to
merge**, seven reviewers and zero actionable/unresolved findings. Receipt:
`docs/reviews/ce-code-review/20261105-integration-5c9e/review.json`.
No source edits followed review. The simplification pass made no changes.
Only the compact receipt, metadata and stage log are committed; detailed
read-only review artifacts remain ignored locally. Review coverage retains
the active-water pixel-oracle, controlled HDR equality, performance, soak and
hardware limits listed below; none establishes an integration regression.

## Limits

The run does not establish an FPS gain, a graph-overhead budget, long-soak
stability or second-GPU behavior. Existing open/incomplete Bistro collision
geometry can let physical props leave the modeled floor; retained functional
wave assertions do not claim perfect containment. Exact HDR comparison uses
the documented controlled material/draw setup, while ordinary rendering and
physical waves are checked separately. The earlier active-water transparent
mesh isolated-pixel-oracle gap remains.

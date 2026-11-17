# App renderer cleanup after the Meta merge

Baseline: `dcf933c1`, branch `codex/bistro-rendering`.
The sole application is `Apps/App`; the separate Meta application was already
removed by the baseline commit. Existing `Meta.*` CVar names and graph namespace
remain compatible. Historical reports are not active application targets.

## Changes

- Decomposed SceneLighting's frame assembly into shadow/probe preparation,
  deferred resource preparation, deferred recording, SSR, and composition.
  Removed the unreachable second SSR-disabled branch and its copy packet.
  Mandatory technique owners no longer have legacy optional-owner branches;
  lazy resources, history resets and lifetime checks remain guarded.
- Extracted geometry upload and draw recording. Named geometry resource/operation
  IDs, batching offsets, upload bucket count, counter/fallback buffer sizes,
  descriptor boundary, lighting dispatch dimensions and camera controls.
  Startup sky and camera preparation are separate methods.
- Removed early-AO plumbing: the sole pipeline always selected AO after GBuffer.
  This removes the unused prepass-normal texture per slot, its PSO/shader setup,
  graph resource, callback and ordering branches. All four AO algorithms remain.
- Removed unused Far/Near mesh-transparency owners and branches. The current
  opaque-particle water mode uses one All WBOIT phase with the same particle order.
- Removed unreachable uncategorized application initialization and tableware
  replication from ConfigureBistroProps. It was always called with categorized=true.
  Generic prop helpers and their independent fixtures retain their contracts.
- Removed unused geometry target parameters, lighting accessors, the unused
  probe argument/result of the post continuation, poster recording switch, and
  grass diagnostic bookkeeping. Probe grass capture uses AppendGrassView too.
- NativeWork::AddOwned shares the recording-state tracker, borrowed command list,
  private-signal rejection and DIRECT/owner recording boundary for water,
  particles and post/UI. Each technique retains its own access normalization:
  their merge/rejection semantics are intentionally different.
- NativeWork, grass and environment recording use binary search over monotonically
  increasing original operation IDs. Graph remapping still restores those IDs.
- Grass appends resource bindings in place for main/shadow/probe views. It no
  longer copies the entire accumulated binding vector or the target vector per
  view; the view matrix transpose is computed once per view. Resource registration
  reuses NativeWork, with a constant diagnostic label to avoid per-lookup strings.
- Command-list pools grow to exact per-queue demand after Begin: payload lists
  plus nonempty before/after barrier lists. Removed the 1024-pairs-per-queue-per-slot
  initial allocation and global-count reservation. Both primary and cloud producer
  callers reserve before Execute. Lists remain persistent at their high-water mark.
- Terminal dependency completeness uses a presence mask instead of a repeated
  linear search. Invalid dependency IDs still fail the existing topology check.

## Style and review

Added `Apps/App/src/.clang-format` and applied it to the substantively edited
sources: Allman braces, tab indentation, explicit control-flow blocks and bounded
line width. The reference is [Epic's C++ coding standard](https://dev.epicgames.com/documentation/unreal-engine/epic-cplusplus-coding-standard-for-unreal-engine).
The project keeps its C++17/STL/DirectX types and existing APIs; this is not an
Unreal dependency or a wholesale public-symbol rename. SceneReflections has only
the required one-line caller change, without whole-file reformatting.

Independent reuse, quality, efficiency and comment reviews were followed by
source review of the implementation. Review/build caught and corrected an obsolete
transparency label expression and the remaining probe grass caller. State boundary
equivalence, graph/resource IDs and reservation ownership were rechecked.

The compiler's topological scheduling algorithm, reflection count validation,
generic physics helpers, numerical solvers, shader algorithms and external SDK
contracts were retained. No attempt was made to replace every literal: shader
binding slots and authored scene data need their own domain-specific review.
Large untouched shadow/water routines remain candidates for further decomposition;
this report does not claim the entire source tree is free of duplication.

## Verification

- Full App Release rebuild PASS, MSBuild `/m:1 /nr:false`,
  `/t:Rebuild /p:BuildProjectReferences=false` after the referenced libraries had
  already built. Log: `Build/app-refactor-release-final.log`.
- Initial sandbox compilation failed with MSVC/PDB C1902. Host build exposed a
  stale caller/object mismatch; corrected caller and full App rebuild resolved it.
  Existing C4244 and external PDB/LNK4099 warnings remain.
- clang-format dry-run/Werror PASS on the 22 formatted files; diff whitespace
  check PASS. SceneReflections' one-line caller edit was reviewed separately.
- One ordinary App process launched without diagnostic arguments. Session log:
  `Build/app-refactor-smoke.log`, 2026-11-17 13:01:35 UTC onward. Bistro loaded,
  Native TAA reported 1280x800, cloud publication reached sequence 61 and physical
  props completed six lift/scatter cycles. No ERROR/FATAL appeared in the session.
  A passive NVIDIA sample during execution showed 98% utilization, not an A/B result.
- Visual smoke is INCOMPLETE: both computer-use entry points failed with
  `unknown or expired node_repl continuation` before launching the app. The CLI
  fallback ran with a hidden window. That owned process was terminated after the
  bounded check; presented image and graceful window shutdown were not verified.
- No Debug build, provider/mGPU matrix, new tests, accuracy checks, profiling or
  performance A/B were performed. Ordinary settings/scene population were unchanged.

CPU work and unused allocations are reduced structurally; no measured FPS gain
is claimed. On-demand list creation moves some allocation to the first use of a
frame slot. Screenshot acceptance and startup/steady-state cost remain unmeasured.

Executable: `Build/bin/x64/Release/App/App.exe`. Local commit only, no push.

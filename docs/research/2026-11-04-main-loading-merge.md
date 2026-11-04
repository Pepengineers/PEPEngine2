# Main loading merge, 2026-11-04

Merged `origin/main` at `672a4ef` into `codex/bistro-rendering` at `3fdd650`.
The common ancestor was `d828aff`; main contributed eight implementation commits
and the PR #54 merge. No patch-identical commits were found by `git cherry`.

## Resolution

| Incoming change | Decision and evidence |
| --- | --- |
| Background startup, loading UI and cancellation | Retain the existing `App::ReportLoading`, `ContinueLoading`, `WaitForLoading` and bounded jobs in `LoadObjSceneFiles`. They already keep the window responsive during imports and support cancellation, Mesh/BC7 caches, loading measurements and the transient-memory budget. |
| `SceneLoadTask`, `PreparedScene`, `WorldLoader::Committer`, GPU progress bar and `ReplaceScene` | Do not add a second loading path. These are architectural alternatives, not exact duplicates: main adds staged scene replacement, numeric progress and time-sliced main-thread publication. Its preparation imports meshes serially with default options and retains all prepared meshes/textures until publication, without our cache settings or transient budget. Its startup also assumes a configured YAML start scene; Bistro constructs a directory-backed scene through the existing bootstrap. These new behaviors are deliberately not enabled by this merge. |
| Centralized YAML document/resource parsing | Retain the existing parser and resource resolution. Main's new resource resolver unconditionally makes paths relative to the process directory; ours also resolves against the world directory and asset registry roots. The new document types primarily serve the omitted staged loader. |
| Default `World::ActiveCamera` | Accept `InvalidEntity` initialization; cover the empty-world case in `LoadingTests`. |
| Cancellation inside mesh conversion | Adapt node, submesh, vertex, face and bone checkpoints to the existing `MeshImportOptions::Continue` contract (false means cancel). Preserve skinning, source node names, native cache and the public importer signature. `LoadingTests` cancels at every observed import checkpoint and rejects partial results. |
| Static mesh event unsubscription | Already present in `RenderModule::UnsubscribeFromWorld`, including GPU state cleanup. Retain that implementation. |
| Register GPU state for components present before subscription | Specific to staged world publication. Our scene manager subscribes the world before loading its components; adding the incoming replay is unnecessary for this path. |
| Non-logging texture lookup | Used by the staged loader; omit the unused API. |

## Validation

- Release/x64 host builds of `App`, `LoadingTests` and `BistroStartupTests`
  succeeded, including rebuilding the final sources after the baseline control.
- Final `LoadingTests` passed: cancellation at every observed import checkpoint,
  native Mesh cache/integrity, texture lifetime/BC7, YAML loading, system order,
  banlist and unload. Empty worlds start with `InvalidEntity` as active camera.
- A control build of the pre-merge production sources at `3fdd650` passed
  `BistroStartupTests` normally once; a second clean control build/run failed
  with the same exit code 2173. The final merged binary passed under a native
  debugger: both Bistro scene parts, 346 props, eight emitters, 90 frames with
  no D3D12 debug-layer errors, image-quality UI and background/focus/resize tests.
- Normal merged-binary runs were unstable: three recorded exit code 2173
  (Windows Application Error exception `0x0000087d` in `KERNELBASE.dll`), and
  another stalled and was terminated. No production rendering or test settings
  were changed to hide this. A debugger run did not reproduce the exception;
  its cause remains undiagnosed. The failure also reproduced with the original
  importer and only the camera initialization enabled, and with both production
  changes removed. Thus this GPU-test instability exists before this merge;
  these checks do not establish reliable normal-launch or long-run stability.
- Sandbox attempts hit MSVC C1902/PDB access and a temporary Mesh-cache write
  failure; the successful build/CPU-test results above are from host runs.
- Builds reported warnings in unchanged renderer/vendor code (C4018, C4267,
  C4828) and debug-symbol warnings LNK4020/LNK4099; tests also report D9025
  for intentionally undefining `NDEBUG`. No toolchain or vendor cleanup is
  included. Debug/x86 builds and a long GPU stability run were not performed.

Artifacts are under `Build/merge-main-*`, including the baseline and diagnostic
stdout/stderr and build logs. This merge does not claim equivalence to main's
staged loader or add its percentage progress display.

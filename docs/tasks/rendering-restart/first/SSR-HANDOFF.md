# SSR implementation handoff — historical snapshot

This handoff records the author state before central verification. The verified SSR is now integrated in the top-level project, `D:/Project/PEPEngine2`, branch `codex/bistro-rendering`; see [the current review](../../../rendering-review.md). The old author checkout was removed after preserving its commits in `codex/feature-ssr`. The restrictions and unchecked status below describe that historical handoff only.

Status: **implemented, unverified; not merge-ready**. The user's later instruction
stopped all builds, tests, GPU fixtures, benchmarks, visual inspection and separate
review/cleanup passes. The main chat owns further verification and revision.

Checkout: `D:/Project/PEPEngine2/Build/Features/SSR`.
Branch: `codex/feature-ssr`, based on `271551e`.
No push or merge into the main checkout. Pre-existing unrelated WIP is excluded.

## Implementation

- AMD FidelityFX SDK v1.1.4 pinned to `c6efa6bf7f2027b3ec94f28578bb5965eabb9e55`.
  Bootstrap verifies archive SHA256 and builds DX12 backend, SSSR and Denoiser;
  no Cauldron. MIT notice retained in `External/FidelityFX`.
- `GDX12SSSRPass`: G-buffer inputs, private motion/roughness preparation, SDK
  dispatch (including Reflections Denoiser), global fallback and single specular
  resolve. Jittered previous VP, signed world normals, reverse-Z, scene cm.
- Runtime `Render.SSR.*` settings registered once in a `.cpp`, Default flags,
  validation, frame snapshot, independent overrides (`-1` inherits presets).
  Enabled, Off/Low/Mid/High, traversal limit/occupancy/mip/rays, roughness cutoff,
  thickness, variance tracing/threshold and denoiser temporal stability.
- Camera cut API `CameraComponent::InvalidateHistory()`, shared history revision,
  resize/re-enable/source-resource reset via SDK context recreation.
- Linear HDR WBOIT composition and HDR XeSS; final Reinhard/sRGB output pass.
  XeSS resetHistory and native command-list cache invalidation are integrated.
- Separate graphics/compute root-signature caches; invalidate SDK binding state.
- `External/FidelityFX/cold-history.patch` changes one denoiser binding: spatial
  and temporal filters read the average radiance produced by current-frame
  reprojection. The unpatched SDK clipped the first frame to black in the fixture.
- Existing tests already added are preserved: `SSRTests.vcxproj` and
  `SSRBistroTests.vcxproj`. Do not treat their presence as verification of final code.

## HDR/probe integration contract

Opaque deferred lighting supplies complete linear HDR including global IBL.
SSSR resolves `litHDR + (SSSRradiance - oldGlobalRadiance) * (F0 * DFG.x + DFG.y)`
only on eligible opaque pixels. SDK RGB includes its miss fallback. Output alpha
is internal denoiser payload, never confidence. Off copies HDR exactly.

The next probe integration must replace the same indirect-specular term in
`PBRLighting.hlsli` and `SSSR.hlsl::Resolve`, and reconcile SDK offscreen fallback
(the SDK accepts one global cubemap). There is no speculative probe interface.
Transparent bottles retain forward PBR/global IBL and WBOIT; they are not opaque
G-buffer SSR receivers or hit geometry. No scene entities/materials were added.

## Evidence and unresolved work at stop

Before the stop instruction, earlier Release builds, CVar tests and analytic GPU
readbacks ran. They found/fixed cold-history clipping and then passed ray-hit/HDR,
single-specular, Off, interior fallback, cutoff, reset, odd-size, jitter and motion
checks. These results do **not** certify the final changes.

The full Bistro run found shared environment/DFG resources left in compute-only
read states before forward WBOIT. The implementation was changed to preserve
combined pixel/compute SRV state, and final tone-map input uses combined SRV too.
That final integration is **not confirmed**: the latest Bistro smoke was stopped
by explicit instruction (owned process 29676). An earlier redirected run produced
no completion output; its native exit status was not retained. Investigate rather
than infer success. Computer-use app approval timed out, so no Bistro visual
inspection was completed.

The denoiser softens constant environment fallback near sky silhouettes/viewport
edges; one fixture measured maximum channel difference 0.110352 at an edge while
interior receivers matched within 0.015. No claim of exact miss preservation at
silhouettes. Long-run stability, full in-flight resource behavior, final full App
build and final Bistro/XeSS/transparent-bottle rendering remain for the main chat.

SDK context memory remains allocated while Off; it is recreated on re-enable.
Earlier fixture timing/memory samples exist under ignored `Build/` but are not
final performance results. No FPS target or Bistro timing conclusion is claimed.

Full CVar defaults, units/ranges, preset values and seams are in `docs/sssr.md`.

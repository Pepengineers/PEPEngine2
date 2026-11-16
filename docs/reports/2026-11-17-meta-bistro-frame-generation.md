# Meta/Bistro frame generation — 2026-11-17

Ordinary Bistro now enables the existing shared renderer frame-generation path.
`Render.FrameGeneration.Provider` remains **0 Off by default** in both engine and
Meta Startup. Values 1/2/3 select FSR FG / DLSS FG / XeFG through existing CVars
and Settings. Unsupported SDK/hardware requests report their reason and fall back
to Off. No new SDK, jitter sequence or independent FG implementation is added.

## Integration

Removed the ordinary (`_opaqueOnly`) host's FG prohibition. Both scene routes
use the same extracted DIRECT prepare/tag packet after reconstruction/display
and before screen UI/Present. It reads composition depth and complete motion.
Depth/motion are exported as NON_PIXEL_SHADER_RESOURCE, retained by frame slot,
and restored as imports on subsequent plans. Existing executor/swapchain drain
handles switching, resize and retirement. No private submit or per-frame CPU wait
is added by this integration.

Frame-slot exports keep resources alive; a separate small published-input set
tracks their NON_PIXEL_SHADER_RESOURCE boundary. Once exported, an input keeps
that boundary until resize: imported published inputs retain the same final
state, and absent inputs are left untouched. The pending union is committed
only after submission; resize clears it after drain. Stale slot exports are not
used as state truth. This also covers a motion
texture temporarily omitted by Native/blur-off before FG is enabled again.

Complete motion and particle feature motion are requested when FG is selected,
including Native/NIS with Motion Blur disabled. FG uses the existing camera data,
unjittered NDC motion and reconstruction history/reset path. The SDK's own
interpolation callback (FSR) or last-present status (XeFG) supplies the generated
frame counter; selecting a provider alone is not evidence of generation.

## Verification

Release build passed. Requested runtime checks are ordinary Bistro (no `--smoke`),
Native TAA at 1280×800, primary NVIDIA GeForce RTX 3060 Laptop GPU (5994 MiB),
three frame slots, unchanged scene budgets. FSR FG and XeFG results are recorded
below. No automated tests or extra mode matrix added.
Native/blur-off, switching/resize and failure fallback receive source review.

## Limits

Existing SDK integration interpolates the whole displayed frame: XeFG UI mode
NONE and no separate FSR HUDless/UI resource. Settings/HUD may show interpolation
artifacts. This stage does not add HUDless composition or new latency/FPS tooling.
The title's FPS remains the application's rendered frame rate, not displayed FPS.
Mesh transparency retains its existing depth/motion limits. A short smoke proves
SDK work and visible output, not artifact-free interpolation in every material.

## Swapchain correction and runtime evidence

The first FSR launch selected the provider and recorded Prepare but produced no
interpolation callback. Short owner diagnostics confirmed ActiveFrame=true,
Suspended=false and Reset=false after initialization. The common FG wrapper was
upgrading the already-created FSR/XeFG proxy through Streamline; that additional
wrapper bypassed the vendor's interpolation layer on Present.

FSR/XeFG now remain the outer presentation swapchain; normal Off/DLSS FG creation
still uses the existing device factory/Streamline path. Removing the second
upgrade made FSR callbacks run and XeFG report interpolated presents. SDK math,
queues and resource ownership are otherwise reused. Temporary diagnostics were
removed; only the first positive SDK-generated-frame log remains.

Verified ordinary run, switching through the real Settings control:

| Provider | SDK evidence | Scene/output |
| --- | --- | --- |
| FSR FG (1) | callback succeeded; GeneratedFrames became 1 at render frame 978 | Visible animated Bistro, 1280×800, Native TAA; no FG error/fallback |
| XeFG (3) | last-present status reported extra frame; GeneratedFrames became 1 at render frame 1559 | Visible animated Bistro, same inputs; no FG error/fallback |
| Off (0) | selected again after both checks | Scene continued, then application closed normally |

Evidence is in `Temp/meta-fg-verified.log` (not committed). Both successful paths
were observed beyond their first interpolation while the scene continued to
animate. The title reports rendered FPS only; no performance A/B or latency
claim follows from these checks. Startup and the output runtime config were
returned to Off. DLSS FG was not launched in this task; existing capability/error
handling remains in place for unsupported hardware. Reconstruction combinations,
resize/pause matrices and longer quality checks were not run. Final published-state
tracking receives source review and Release rebuild; the successful interpolation
checks are not repeated solely for this bookkeeping correction.

Final Release Meta build passed (`Temp/meta-fg-build-delivery.log`); the shared
renderer/App Release build also passed (`Temp/meta-fg-build-final.log`). Existing
external conversion and missing Jolt PDB warnings remain. Final source review
and `git diff --check` passed. Unrelated painting/test changes stay outside this
task and its local commit. No push performed.

References checked against the integration:
[FSR FG API](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/main/Kits/FidelityFX/docs/techniques/frame-interpolation-api.md),
[FSR swapchain/UI notes](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/main/Kits/FidelityFX/docs/techniques/frame-interpolation-swap-chain.md),
[XeFG developer guide](https://www.intel.com/content/www/us/en/developer/articles/technical/xess-fg-developer-guide.html).

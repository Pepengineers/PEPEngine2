# S2 — strict selected-shadow routing

2026-11-10. Worktree `shadows`, branch `codex/mgpu-shadows`, base `57df869402f288fa7b56a49d8c43fc9c20f680a2`. The base contains P4 `50ee64fb`, accepted P2d and S2R.

## Implementation

- The technique defaults off. The controller admits complete sun, point and spot groups by stable S1 key. A missing or stale replica leaves its whole group primary. The secondary atlas packs only admitted views.
- The placed mesh raster uses retained S2R IA bindings, the existing masked shadow pixel shader, forward depth comparison and raster bias. Its IA-only vertex variant accepts already deformed 56-byte vertices. The same frustum planes run on CPU against frozen bounds. Native grass has a separate prepared depth batch with matching per-view patches and issued readiness.
- Local static and final depth are separate stages. Cached depth is matched to its backing, light and view inputs. The successful receipt commits the cache. A failed recording leaves legacy fallback available. An uncertain submission faults the domain.
- Secondary DIRECT packs typed `R32_FLOAT` depth. P3b Exact returns the payload through both COPY queues. Primary DIRECT restores the mapped groups through `SV_Depth` and preserves other regions. The import reader retires on restore and the primary atlas stays retained through the actual legacy tail.
- A selected destination subresource gets a full DSV clear before its first restore on that backing. The successful receipt records that initialization. Later restores write the selected depth directly. Dirty local raster clears a complete slice and dirty sun raster clears its complete four-view group.

## Verification

All accepted jobs ran serially with the S2 lease and an owned-process guard. Both builds used v145 Debug/x64, `/m:1 /nr:false`, and the local P0 `/Z7` override. The existing `Build/bin` junction remained in place. Each fixture used its own minimal INI.

| Check | Result | State in `Build/S2` |
| --- | --- | --- |
| Independent pack/restore build | PASS | `build-independent-final.json` |
| CPU whole-group layout | PASS | `layout-cpu-final.json` |
| Native pack/restore with debug layer | PASS | `depth-debug-final.json` |
| Native pack/restore with GBV | PASS | `depth-gbv-final.json` |
| Routing build with P4/P2d/S2R | PASS | `build-routing-final.json` |
| CPU routing and default-off | PASS | `routing-cpu-final.json` |
| Strict routing on two physical adapters, debug layer | PASS | `routing-two-gpu-final.json` |
| Strict routing on two physical adapters, GBV | PASS | `routing-two-gpu-gbv-final.json` |

The independent fixture verified one-layer spot, mixed point and spot, and all four sun quadrants. It exercised relocated slices, edge texels, repeated restoration of initialized depth, stale-group rejection and every sentinel texel in unselected local slices. The routing fixture exercised native masked and opaque IA raster, P3b Exact return, all/sun/all-local/specific point and spot, a shared tail, stale generation, missing grass admission, static cache reuse and a thrown pre-submit recorder followed by successful retry. All eight jobs had empty stderr and matching input fingerprints before, after and at review.

The earlier failures remain in the logs. `depth-debug.json` found that the graphics SRV table needed `ALL_SHADER_RESOURCE`. `build-routing-accepted.json` found a missing complete `GDX12RenderPass` include. `routing-two-gpu-v2.json` found that a fresh DSV required whole-subresource initialization before partial access. The accepted route records initialization only after the issued receipt and the repeated fixture checks unselected slices.

`Build/S2/processes-final.json` lists runner roots and descendants with `OwnWorkloadCount=0` and `ChecksComplete=true`. `validation-summary.json` contains source and executable fingerprints. The S1 accepted executable hashes still match.

- `MGPUShadowDepthTransferTests.exe` SHA-256 `4dbc2c50eea815218d81f090b722e4e414242cfdda439739f5c32e18edc24a7d`
- `MGPUShadowRoutingTests.exe` SHA-256 `2ecfc191dd5947b5438e0da23797946618800b48ea46f4bffead51f487c64d3b`

## Integration boundary

Production project registration, shared pipeline/P2d callbacks and live scene replica wiring remain with the integrator. The focused two-adapter fixture uses real shadow raster and transfer with small test geometry. Actual live secondary skinning, native grass, deferred/OIT/probe parity and performance require separate acceptance.

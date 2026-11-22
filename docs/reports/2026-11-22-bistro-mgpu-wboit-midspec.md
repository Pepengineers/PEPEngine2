# Bistro MGPU transparency on MidSpec

## Review fixes and release smoke

Release x64 built with 0 errors. Two ordinary Bistro runs used `Render.PrimaryAdapter = 0` and `App.Particles.Adapter = -1`, with `App.WBOIT.Adapter = -1` for the local run and `1` for Exact on the Arc B570. The runner observed ordinary frame publication on RTX and two completed layer consumers in Exact mode. Neither smoke log contained error or critical entries. The runner stopped both hidden processes after capturing completion.

| Exact frame | Transparent mesh commands after frustum culling | RTX to Arc, depth bytes | Arc to RTX, layer bytes | Arc mesh / resolve / export fence | RTX import / apply / frame fence |
| --- | ---: | ---: | ---: | --- | --- |
| 1 | 176 | 1,443,776 | 2,887,552 | 26 / 31 / 34 | 500 / 501 / 511 |
| 2 | 177 | 1,443,776 | 2,887,552 | 36 / 41 / 44 | 698 / 699 / 709 |

The render size was 752 × 470. CPU-to-Arc geometry upload was 46,307,880 bytes on frame 1 and zero on frame 2. The resident mesh cache includes off-screen meshes, while the submitted draw list uses the display culling plane equations. The logged command counts describe draw submission and are not pixel counts or a timing comparison.

A transparent skinned mesh or a transparent emitter configured for the interaction field selects the primary WBOIT path for the frame and reports the reason. A change of owner preserves consumed burst totals by emitter and reset revision. When the diagnostic water draw will overwrite scene depth, the primary WBOIT path captures the same completed opaque depth version in a GPU0-local texture. Source images used to recreate diffuse-plus-opacity textures have temporary CPU lifetimes. These review paths were checked in source. There were no special-case runs for switching owners, fallback, water or texture memory.

Local logs are `Build/bistro-wboit-review-release.log`, `Build/bistro-wboit-review--1-smoke.log` and `Build/bistro-wboit-review-1-smoke.log`.

## Shared shading and depth-only transfer (b9af2897)

The Release x64 app built successfully. Both modes use the same directional light and ambient shader for transparent meshes, sample opaque scene depth, and leave the separate transparent particle motion texture disabled. The local run used `App.WBOIT.Adapter = -1`. The Exact run used `App.WBOIT.Adapter = 1`. Both used `Render.PrimaryAdapter = 0` and `App.Particles.Adapter = -1`. The local run completed ordinary frames on the RTX 5050, including the publication recorded at sequence 61.

The Exact run used the Intel Arc B570 as transparency owner at 752 × 470. Arc built its geometry and draw inputs from CPU data, loaded its textures from the same asset sources, and ran transparent particle simulation and rasterization. Both recorded frames supplied 768 transparent mesh commands, three weighted and four additive emitters. There were no SortedAlpha emitters in this smoke.

| Frame | RTX to Arc, depth footprint (bytes) | Arc to RTX, layer footprint (bytes) | Arc mesh / resolve / export fence | RTX import / apply / frame fence |
| --- | ---: | ---: | --- | --- |
| 1 | 1,443,776 | 2,887,552 | 28 / 33 / 36 | 500 / 501 / 511 |
| 2 | 1,443,776 | 2,887,552 | 38 / 43 / 46 | 698 / 699 / 709 |

The first frame uploaded 46,307,880 bytes of mesh data from CPU into Arc-local buffers. The second frame reused those buffers. No geometry, textures, particle pool data, motion, opaque HDR or shadows went between the GPUs. The log reported no runtime errors on either successful run. The harness stopped both hidden instances after useful evidence was available. The local harness reported incomplete because it waited for a fourth world-canvas log entry and that logger emits only three per device. The publication at sequence 61 independently shows the local run continued rendering. The Exact harness reported completion before `Stop-Process`.

No visual comparison or performance measurement was made. Local evidence under `Build/` is `bistro-wboit-parity-release.log`, `bistro-wboit-parity--1-smoke.log` and `bistro-wboit-parity-1-smoke.log`.

## Initial implementation (5b4e5f5f)

Release x64 built from `Apps/App/App.vcxproj` on November 22, 2026, with 0 errors and 143 warnings. One short Bistro smoke exercised Exact transparency. The runtime identified GPU0 as NVIDIA GeForce RTX 5050 and GPU1 as Intel Arc B570. The runner temporarily appended these settings to the Release startup config and restored both startup and active configs on exit.

```ini
Render.PrimaryAdapter = 0
App.WBOIT.Adapter = 1
```

The smoke used a 752 × 470 render size with DepthNormals and particle motion enabled. On both recorded frames the scene supplied 768 transparent indirect command entries and 3 weighted, 4 additive and 0 SortedAlpha emitters. These are input counts and do not count fragments that passed culling or depth testing. The Bistro inventory also listed 256 glass objects for each quality level.

The Arc B570 rendered the transparent meshes and particles, resolved weighted accumulation (`RGBA16_FLOAT`) and revealage (`R16_FLOAT`), then exported premultiplied RGB and transmittance `T` (`RGBA16_FLOAT`) and particle motion (`RGBA32_FLOAT`). The RTX 5050 imported and applied the layer to its local HDR. Completed frame fences were observed after layer application on both frames.

| Frame | GPU1 mesh / resolve / export | GPU0 import / apply / completed frame |
| --- | --- | --- |
| 1, first | 2 / 5 / 8 | 506 / 507 / 517 |
| 2, following | 10 / 13 / 16 | 710 / 711 / 721 |

The runtime reported the following cross-adapter copy footprints in bytes, including copy padding.

| Transfer | Frame 1 | Frame 2 |
| --- | ---: | ---: |
| GPU0 → GPU1 total | 1,026,686,948 | 330,127,844 |
| Opaque prepass depth | 1,443,776 | 1,443,776 |
| Opaque HDR background | 0 | 0 |
| Geometry | 704,476,172 | 8,182,720 |
| Bindless textures | 18,610,904 | 18,610,904 |
| Shadows | 265,027,584 | 265,027,584 |
| Other inputs and padding | 37,128,512 | 36,862,860 |
| GPU1 → GPU0 total | 8,542,720 | 8,542,720 |
| Transparency layer | 2,887,552 | 2,887,552 |
| Motion and inter-resource padding | 5,655,168 | 5,655,168 |

Neither frame reused shadow data already resident on the Arc. Frame 2 is the first recorded frame after bootstrap. Its transfer totals describe this run and do not establish a steady-state cost.

**Smoke result:** startup, GPU work and consumption of the layer completed without error or critical entries in the runtime log. The runner launched a hidden window and ended the session after observing two completed frame fences. It attempts `CloseMainWindow` and falls back to `Stop-Process`. The recorded process exit was `-1`, which caused the runner to return `1`. The runner did not record which shutdown path it took, so a clean shutdown is unverified. No screenshot or fragment readback was collected. The fences establish that the layer reached the frame, and the log cannot independently confirm the appearance of glass or particles.

The combined path without DepthNormals and the SortedAlpha blend were reviewed in code. Water, pixel accuracy and performance were outside this smoke.

Local artifacts under `Build/` are `bistro-mgpu-wboit-release.log`, `bistro-mgpu-wboit-smoke.log` and `Invoke-BistroWboitSmoke.ps1`. The external reference for the two-GPU WBOIT division of work was [Multi-GPU WBOIT](https://github.com/WorryWarrior/Multi-GPU-WBOIT).

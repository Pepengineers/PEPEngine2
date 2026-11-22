# Bistro MGPU transparency on MidSpec

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

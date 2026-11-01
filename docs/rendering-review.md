# Central rendering integration review — 2026-11-01

Current workspace: `D:/Project/PEPEngine2`, branch `codex/bistro-rendering`. The reviewed code and full Git history are integrated into the top-level project by merge `a7f5781`; the original reviewed code commit remains `353a585`. Nested feature/review clones were removed after preserving every branch, commit and new LFS asset. No push/PR was performed.

The original technique checks below ran before migration; their small logs/images were retained under `Build/RenderingReview`. The migration also builds/runs the top-level project directly, with separate logs under `Build/RepositoryMigration`. Original temporary checkout paths in historical logs are provenance, not live working directories. The second-group chats are stopped, and their unverified code is preserved in ordinary branches of this same top-level Git.

## Inputs and ownership

Root requirements base: `4d7b19ca5948c66601294ad1878208e9bd02ced4`. The actual final author messages were read with `read_thread`, including Reflection's later user correction: only Enabled/Quality are global; capture fields belong to each component. Source commits imported from the five local clones:

| Feature | Source commits |
|---|---|
| AO | `849c31d`, `ffa60e3` |
| SSR | `8774481` |
| Reflection | `5c6c802`, final component correction `16d16b2` (historical validation document retained) |
| DoF | `66a4708`, `97ed6e3` |
| Motion Blur | `2742dfe` |

The old cancelled implementation was not restored. Author validation claims were treated as historical, not as acceptance of the integrated code.

The user clarified that the remaining WIP was entirely agent-created. Warning/MSBuild changes are committed as `ffb529e`; allocator research as `b021641`. Before deleting the copies, residual changes were checked against the committed baseline/canonical files: no unique uncommitted feature edits remained. All imported branch HEADs and new LFS asset objects were verified in the top-level Git. Physical SHA256 comparison of all **414 Bistro files** against the original root passed; the hash inventory is retained in `Build/RenderingReview/bistro-sha256.json`. No assume-unchanged workaround was added to the top-level project.


## Final pipeline and fixes

`clear → environment → GPU culling → depth prepass → [early AO] → CSM → local shadows → probe capture → opaque G-buffer → [late AO] → deferred HDR → SSSR/denoiser/specular resolve → transparent WBOIT → HDR composition → DoF → motion blur → HDR XeSS → one tone map/sRGB output`.

- Added the requested depth prepass by reusing the existing opaque draw/root/indirect path. `Render.AO.ExecutionPoint=0` runs AO immediately after prepass; `1` (default) runs it immediately before lighting. Both are runtime C++/INI controls. Early mode writes depth and the normal-mapped world normals AO needs. Late mode uses separate DepthVS/DepthPS, **zero color targets and no normal/material G-buffer evaluation**, with the same masked alpha-test. G-buffer reuses the same depth resource, Equal comparison and zero depth writes. CSM/local shadows start after prepass (and early AO when selected). Normal storage is retained for runtime switching but is untouched by the late prepass.
- Resolved the actual AO/probe binding collision: deferred t12/t13 are probe radiance/metadata, t14 is AO. Preserved AO only on diffuse indirect lighting; direct/specular/emissive/baked remain neutral.
- Connected all five techniques rather than keeping the last branch's postprocess chain. Linear RGBA16F survives WBOIT, DoF, motion blur and XeSS; only final output tone maps. Removed the duplicate unused ToneMapping shader and duplicate FidelityFX preparation wrapper/imports/project items.
- Replaced SSSR's global-only miss integration with the same local/global prefiltered incident radiance used by PBR. One receiver-grid RGBA16F fallback is generated on GPU; a small pinned SDK patch loads it at each ray origin in classify/intersect. Resolve subtracts that same fallback and applies the split-sum BRDF once. The real AMD ray marcher, confidence blending and Reflections Denoiser are retained. Denoiser alpha is never treated as confidence.
- Consolidated SSSR/denoiser/DoF into one pinned FidelityFX SDK build, checked archive SHA256 and idempotent source patches. Fixed PowerShell 5 checksum/expected patch-check handling and missing public DXGI linkage. No Cauldron or new framework/registry was added.
- Reconciled camera cut/projection/resize reset, previous/current jitter conventions and separate camera-motion versus upscaler invalidation. Positive raster jitter is shared by actual raster, XeSS, AO preparation and SSSR's UV-history conversion. Camera.ResetHistory is consumed after calculating the reset, not before. SDK native state invalidation preserves separate compute/graphics roots. SSSR also resets on environment revision without probes, and safely flushes before dispatch when the configured frame ring exceeds the SDK's queued-frame capacity.
- Preserved per-probe resolution/mode/faces/sample/update settings and atomic publication. GPU tests publish two probes with different resolutions/modes/budgets; settings are not globalized.
- Made the Motion Blur INI fixture create/check its output directory. Reflection's additional INI oracle runs before TestEngine freezes startup configuration; the production ConsoleModule startup-only INI contract was preserved. Added actual INI override oracles for SSR/Reflection and AO execution timing, queued SSSR submission, mixed-probe and prepass readback oracles. The manual Bistro transition fixture now pumps window messages and logs each frame around resize.

## Reproduction and completed checks

Run from `D:/Project/PEPEngine2`. Builder: `C:/Program Files/Microsoft Visual Studio/18/Professional/MSBuild/Current/Bin/MSBuild.exe` (18.8.2); Release/x64, MSVC 14.51.36231. The existing host workaround was kept local:

```powershell
$env:_CL_ = '/Z7 /GL-'
$builder = 'C:/Program Files/Microsoft Visual Studio/18/Professional/MSBuild/Current/Bin/MSBuild.exe'
& $builder Apps/App/App.vcxproj /p:Configuration=Release /p:Platform=x64 /p:PreferredToolArchitecture=x64 /m:1 /nr:false
```

App and these focused projects built successfully with the same flags: AOTests, SSRTests, ReflectionProbeTests, DepthOfFieldTests, MotionBlurTests, ObjectDataJobsTests, LightingGpuTests and BistroDofSmoke. After App/dependencies built, independent fixture builds used `/p:BuildProjectReferences=false`. Existing conversion/dependency-PDB warnings remain; no global toolchain changes were made.

| Run | Result and oracle |
|---|---|
| `AOTests --cpu` | CVar/INI/range/finite/override validation including ExecutionPoint |
| `SSRTests --cpu-only` | CVar/presets/actual INI override/Off |
| `ReflectionProbeTests --cpu` | two global CVar/INI controls; per-probe presets, author validation, explicit settings preserved |
| `DepthOfFieldTests --cpu` | thin-lens CoC sign/units/RH reverse-Z at four FOVs/resolutions and runtime settings |
| `MotionBlurTests --cpu` | settings/ranges/finite/INI/manual overrides |
| `AOTests` | all four real algorithms, plane/step/corner, neutral Off/background/fade, native settings with GPU effect, queued frame-slot readbacks, 1×1/17×13/321×241/1280×720, HDR/XeSS/reset/tone map |
| `AOTests --gpu-validation` | focused AO GPU-based validation of all algorithms/resource paths; vendor XeSS subtest excluded and checked separately with normal debug layer |
| `SSRTests` / `--five-frames` | distinct red/green HDR hits; no double specular; local/global miss fallback; cutoff; cold/warm/cut/re-enable/resize/jitter/camera/object motion; queued submissions and environment reset; debug layer |
| `ReflectionProbeTests --debug` | actual baked color+matching depth hiding dynamic geometry, dynamic overlay, cache/material/world invalidation, complete publication, off-camera shadows, roughness, overlap/parallax, queued frames, opaque/transparent PBR, mixed 32/64px probes; prepass depth/normal identity and late normal-target non-write oracle; HDR/XeSS/odd resize/tone map |
| `ReflectionProbeTests --capture-gpu-validation` | focused GBV capture/cache/shadows/GGX/publication, all quality budgets; excludes separately checked vendor XeSS/PBR section |
| `DepthOfFieldTests` / `--five-frames` | exact Off/focus, near/far blur, physical CoC, HDR bokeh, colored silhouette/leakage, ring merge, FOV/quality/override, queued slots/odd resize/re-enable, native/output XeSS grids and final tone map; clean debug layer |
| `MotionBlurTests` | exact stationary/Off, sign symmetry, foreground spreading/occlusion/different directions, sky rotation vs translation, shutter/radius/quality, cut/resize/re-enable/odd grids/queued slots, analytic XeSS convergence and same-list final tone map; clean debug layer |
| `LightingGpuTests` | AO diffuse/direct/specular/emissive/baked energy invariants, IBL/cubemap and actual point/spot lighting/shadow readbacks |
| `ObjectDataJobsTests` | real 1/4-thread ECS/object/camera jobs, frame slots, immutable indirect templates, buffer growth and reflection classification/material updates |
| `BistroDofSmoke` | real existing bottle camera; 32 frames each for DoF Off/Low/Mid/High, full five-technique HDR pipeline, finite HDR framebuffer readbacks and visual inspection |
| `ObjectDataJobsTests --render-bistro <absolute Bistro path>` | first run passed 36 frames, 346 existing props, camera/object motion, alternating early/late AO and all four implementations, effect settings, cut and odd resize; clean debug layer. Final message-pump rerun also passed every frame 0..35 with 346 existing props, clean debug layer. |

Every GPU run was serialized with `Local\PEPEngine2FeatureGpuValidation`. AO/SSR/Reflection/DoF/MotionBlur/BistroDofSmoke acquire it internally. LightingGpuTests and ObjectDataJobsTests were wrapped externally for the accepted runs:

```powershell
$gate = [System.Threading.Mutex]::new($false, 'Local\PEPEngine2FeatureGpuValidation')
$null = $gate.WaitOne()
try { & Build/bin/x64/Release/ObjectDataJobsTests/ObjectDataJobsTests.exe --render-bistro 'D:/Project/PEPEngine2/Assets/Scenes/Amazon Lumberyard Bistro' }
finally { $gate.ReleaseMutex(); $gate.Dispose() }
```

Original logs and generated image artifacts are retained in `Build/RenderingReview/`. Key logs: `gpu-AO.log`, `gpu-MotionBlur.log`, `final-gpu-SSRTests.log`, `final-gpu-ReflectionProbeTests.log`, `final-gpu-DepthOfFieldTests.log`, `focused-gbv-AOTests.log`, `focused-gbv-ReflectionProbeTests.log`, `connected-LightingGpuTests.log`, `connected-ObjectDataJobsTests.log`, `bistro-visual.log`, `final-bistro-transitions.log`. Current build logs include `final-build-App.log`, `ini-build-SSRTests.log`, `ini-build-ReflectionProbeTests.log` and `build-ObjectDataJobsTests-final.log`.

Two attempted full GBV runs were stopped **only in owned REVIEW processes** while spending minutes instrumenting vendor XeSS (Reflection PID 24420, AO PID 39800). Their incomplete runs are not counted as passes. Focused GBV and complete ordinary-debug paths above passed. No foreign process was terminated. The initial Bistro transition run completed successfully on its own; no Bistro process was killed.

## Cost and image evidence

GPU: NVIDIA GeForce RTX 3060 Laptop. These are isolated-fixture GPU timestamps on an unchanged workload per technique/preset, excluding CPU load, shader/SDK initialization, presentation and readback. Ordinary debug layer was enabled for listed measurements; GBV timings are not used as performance evidence. Algorithms have different fixtures, so rows are not a ranking of equivalent image quality and do not add up to a Bistro frame budget. No FPS target was supplied.

| Effect / grid | Low / Mid / High GPU ms | Memory boundary |
|---|---|---|
| SSAO 1280×720 | 0.432 / 0.686 / 1.132 | process-local AO fixture residency ≈88.8 MiB |
| HBAO 1280×720 | 0.733 / 1.427 / 2.413 | same AO fixture |
| XeGTAO 1280×720 | 0.314 / 0.408 / 0.584 | same AO fixture |
| CACAO 1280×720 | 0.364 / 0.455 / 0.508 | AO fixture residency ≈99.0 MiB with native CACAO resources |
| SSSR+denoiser+resolve 1280×720 | 0.927 / 0.901 / 1.288 median | process-local fixture residency ≈167.2 MiB; Low/Mid ray overrides/denoiser overhead mean monotonic time is not guaranteed |
| DoF 1280×720 | 0.165 / 0.244 / 0.390 median | process-local fixture residency ≈74.6 MiB |
| Motion blur 1280×720 | 0.445 / 0.561 / 0.702 | its five textures allocate 15,859,712 bytes |
| Motion blur render grid 985×554 →1280×720 | 0.279 / 0.349 / 0.420 | five textures allocate 10,354,688 bytes |
| Probe capture+sun/GGX/publication, own preset resolutions 64/128/256 | 0.023 / 0.072 / 0.742 mean | 5,701,632 / 9,306,112 / 23,855,104 texture bytes; excludes descriptor heaps/frame buffers/separate local-shadow allocations |

Repeated uninstrumented-vs-instrumented runs can move these laptop timings with clocks/load; keep the workload and measurement boundary when comparing changes. Quality changes real rays/rings/slices/samples/faces or resolution; explicit author overrides are intentionally retained.

Actual-image ordering checks: motion blur at render grid saved cost above; analytic XeSS static/moving 16-frame HDR MAE was 0.00073/0.00157. DoF native/output-grid variants differ materially (mean HDR difference 0.493 in the aggressive synthetic fixture) and output-grid DoF costs more; depth/color grids stay aligned before XeSS rather than resampling scene depth without a validated silhouette contract. Bottle readbacks demonstrate real DoF changes through the integrated chain. The four actual Bistro images are `Build/RenderingReview/Visual/bistro-q0.png` through `bistro-q3.png`; the contact sheet `bistro-quality.png` was inspected (top-left Off, top-right Low, bottom-left Mid, bottom-right High). Synthetic probe/AO fixtures are not claimed as Bistro visual acceptance.

## Material limitations

- Single opaque depth does not describe multiple transparent glass layers. Strong f/0.7 DoF in the bottle inspection visibly separates sharp transparent details from their blurred opaque background. WBOIT is approximate; refraction and glass-in-glass capture are not implemented. Transparent surfaces receive PBR/probe fallback but are not SSSR receivers/hit geometry.
- Baked probe receivers retain static-only shadows; moving casters require dynamic capture to update shadows on static receivers. Captures exclude transparency. Probe correctness is validated in isolated GPU fixtures because adding probes/entities to Bistro was forbidden.
- Official XeGTAO projected-normal/horizon approximation darkens oblique slices of an otherwise unoccluded 90-degree fixture (minimum 0.753, mean 0.921; center >0.98). This algorithmic error is recorded, not hidden by calling it exact ground truth. SSAO/HBAO/CACAO were neutral on that plane. Viewports below vendor-supported working sizes use documented neutral/bypass paths.
- SSSR's denoiser is approximate near silhouettes (constant-global miss maximum error 0.110 at the silhouette; interior receivers satisfy the stricter oracle). The HDR local-miss oracle measured max error 0.0078125 normalized by incident radiance, below its 2% bound; an incorrect global fallback differs by 0.1..1.9 radiance.
- No prolonged stability/thermal/memory-soak or target-FPS claim, multi-GPU/SSAA validation, or full vendor-XeSS GBV completion. Complete ordinary-debug XeSS/HDR paths and focused technique GBV did pass. Existing dependency PDB/conversion warnings remain.

Second-group implementation is authorized only after final first-group completion. Its global controls remain Enabled/Quality only; all other settings are component-owned. New authors must preserve both prepass paths and this HDR/specular contract and leave their own results explicitly unverified.

## Second-group launch

First-group integration code commit: `353a585a529d46002a506e1d7e98d66cca55d397`. Six separate full local clones were created from that exact commit after the checks passed. Each inherited only the original warning/research WIP, separately recorded in Build/BASELINE-WIP.txt. Model gpt-6.1-sol / thinking high. The full technique packet, common integration contract, both prepass paths, component-only settings rule and implementation-only/no-verification instruction were sent to every chat. No further confirmation was needed. `wait_threads` confirmed all six active/inProgress after real creation.

| Feature | Thread ID | Local clone / branch |
|---|---|---|
| SKY | `01a19738-1edc-7ec3-8ccc-6606e9d2a492` | `D:/Project/PEPEngine2/Build/Features/SKY` / `codex/feature-sky` |
| GRASS_WIND | `01a19738-6e1d-77e2-a8ef-8540117a9bd5` | `D:/Project/PEPEngine2/Build/Features/GRASS_WIND` / `codex/feature-grass-wind` |
| PARTICLES | `01a19738-99c4-76d1-9829-a740725bdcb5` | `D:/Project/PEPEngine2/Build/Features/PARTICLES` / `codex/feature-particles` |
| FLUID | `01a19738-bb08-7fe2-b4d8-aa306dade5ff` | `D:/Project/PEPEngine2/Build/Features/FLUID` / `codex/feature-fluid` |
| SKINNING | `01a19738-d751-7fe3-85a7-fff273932cf3` | `D:/Project/PEPEngine2/Build/Features/SKINNING` / `codex/feature-skinning` |
| CLOUDS | `01a19739-011e-7770-97d7-c50bb7b527b5` | `D:/Project/PEPEngine2/Build/Features/CLOUDS` / `codex/feature-clouds` |

Their future code is not part of the accepted first-group baseline and must be centrally reviewed separately.

## Repository normalization

By the user's later instruction all code/history now belongs to the top-level Git. `a7f5781` merges the reviewed first group; feature branches preserve the author commits, including all six unverified second-group implementations. All six chats acknowledged STOP/idle before their checkouts were removed. RUNS.json and the task README give their current branch names/HEADs. Two older build snapshots (lighting-validation and iteration-commits/1) contained no unique files outside the root or existing Git history and were also removed. No feature source or new LFS asset was discarded. The original second-group launch table above records historical paths only; current work must use the root project.

Top-level relocation verification: Release/x64 App and all five focused projects built with `_CL_=/Z7 /GL-`. AO, SSR, ReflectionProbe, DepthOfField and MotionBlur CPU and ordinary GPU runs all passed from `Build/bin/x64/Release`. ReflectionProbe specifically confirmed depth-only/depth+normal identity, no late-prepass normal writes and Equal G-buffer reuse. Logs: `Build/RepositoryMigration/root-*-build.log`, `root-*-cpu.log`, `root-*-gpu.log`. Only the root `PEPEngine2.sln` remains; all 12 feature clones and both old project snapshots were removed. The snapshot junctions to root Assets/External were removed without traversing their targets.

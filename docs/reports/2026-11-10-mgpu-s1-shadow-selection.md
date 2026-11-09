# S1 — stable shadow selection and output mapping

2026-11-10. Worktree `shadows`, branch `codex/mgpu-shadows`, base `97f4fac4e4c4767c2495a0544c3cbce506780857`.

## Renderer contract

- `CollectWorldLights` captures local keys as ECS entity slot plus a process-local 64-bit light lifetime. It stamps the key before sorting lights and assigning atlas slices. Two unused local constant words carry the token bitwise through the existing CPU snapshot and upload path. The 464-byte C++/HLSL stride and all field offsets are unchanged.
- The caller audit found default construction and field mutation in scene setup and test fixtures. The dense ECS pool moves elements on reallocation and swap removal. Moves preserve the lifetime, copy construction gives the copy its own lifetime, and copy assignment preserves the destination identity. No scene serialization stores the token. Synthetic constants with a zero generation still draw through the baseline but are ineligible for transfer selection.
- `DescribePreparedShadow` and `DescribePreparedShadows` describe what the owning pass prepared, including reused caches. Groups carry the same raster regions and matrices as the draw, light and scene/caster revisions, and the cascade split and texel metadata for the sun. Local output reports static-only or final depth. Atlas slices are current destinations. A local cache rebakes when the generation or slice ownership changes.
- Selection supports all, sun, all local and a specific local key. View groups contain all four cascades, all six point faces or the one spot face. Input matching compares the whole group's depth inputs and extents. The S2 caller still has to prove payload completion and apply the current atlas mapping. Resolving a configured entity slot to a key belongs to explicit selection, so the same slot cannot silently retarget after recycling.

## Verification

| Check | Result | State in `Build/S1` |
| --- | --- | --- |
| `git diff --check` on S1 paths | PASS | Task-only diff |
| Debug/x64 standalone `MGPUShadowSelectionTests` build | PASS | `build-selection-accepted.json` |
| CPU selection, ECS delete/recreate and dense move/copy, spot remap, sun matrices/camera cut, static and dynamic validity | PASS | `selection-cpu-accepted.json` |
| Debug/x64 `LightingGpuTests` build including renderer and App.Base | PASS | `build-lighting-accepted.json` |
| `LightingGpuTests` on NVIDIA RTX 3060 Laptop with D3D12 debug layer | PASS, empty stderr | `lighting-debug-accepted.json` |
| Runner descendants and immutable inputs | All four accepted jobs exited zero, fingerprints matched, zero owned processes | `validation-summary.json`, `processes-final.json` |

The GPU smoke checked prepared sun and local output groups, generation-driven rebake of both local depth layers, unchanged cache reuse, and existing depth readback, deferred and OIT shadow sampling. It also passed the existing IBL, AO energy and clustered light checks through 769 lights. This was a single-adapter correctness run. Transport, replica execution, GBV, the interactive App and performance were outside this verification.

Builds and tests ran serially through `peengine-perf/scripts/run_job.py` with the S1 lease. The standalone test uses the production collector instantiated with only the two light collection components. The initial test target instantiated the complete world and failed to link unrelated sky validators. The first GPU launch required hydration of the existing DDS LFS object after verifying its SHA-256 against the checkout pointer. The subsequent fixture failure came from expecting two lights during an existing test of a single active slice. The accepted fixture checks the actual enabled lights, face count and slice in each case.

`Build/S1/validate.ps1` records the commands and uses `/m:1 /nr:false`. The full build used local copies of Agility SDK, FidelityFX SDK, Jolt and LFS runtime dependencies plus the P0 `/Z7` override. The GPU run used an immutable `[ConsoleVariables]` seed and separately saved the resulting test INI in `lighting-debug-accepted.Engine.after.ini`. The source config was left untouched.

Accepted Debug executable SHA-256:

- `MGPUShadowSelectionTests.exe` — `235c78a1afa66cd6f0cfeee17281630ee5e3df372a2ec0d514b1b7040988beb4`
- `LightingGpuTests.exe` — `49d73474d1052924227034e333869b1a9102fb90439c529d1fce1a8663a417e3`

## S2 integration boundary

The selected secondary invocation needs a canonical scene revision and device-specific geometry/index buffers, indirect templates, materials and bindless alpha descriptors, frame transforms, skinned deformation and application grass batches. The primary and secondary compare the same captured revisions. Each device retains its uploads and prepared scene until the relevant receipts complete. Legacy secondary frame slots alone do not establish this ownership.

Secondary DIRECT rasterizes its depth maps. Pack selected regions through their depth SRVs to typed `R32_FLOAT`, transfer complete per-light groups under Strict, and restore only matching primary regions via `SV_Depth`. Order restoration against readers of the whole atlas. Dynamic probes consume the common local maps. Baked probes retain their independent static-only maps and probe sun depth. Integration must keep those bindings consistent with deferred lighting and transparency.

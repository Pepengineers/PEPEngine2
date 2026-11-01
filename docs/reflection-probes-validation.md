# Reflection validation — 2026-11-01

> Central REVIEW acceptance and remaining limits: [rendering-review.md](rendering-review.md). Historical branch/handoff statements below are retained as history.

Implementation: `5c6c802` on `codex/feature-reflection`, in `D:/Project/PEPEngine2/Build/Features/REFLECTION`. The main checkout and other feature copies were not modified. Baseline warning WIP remains unstaged and was excluded from the implementation commit.

## Completed checks

- App Release/x64 build passed with MSBuild 18, PreferredToolArchitecture=x64, `/m:1 /nr:false`, `_CL_=/Z7 /GL-`. Dependency PDB and existing compiler warnings remain.
- ReflectionProbeTests and ObjectDataJobsTests Release/x64 targets build successfully.
- `ReflectionProbeTests.exe --cpu` passed against the complete updated Engine.ini. It checks actual CVar registration/validation, Off/Low/Mid/High, persistent overrides and author field validation, including nonfinite/overflowing bounds.
- An INI value `Render.Reflection.Quality=4` was rejected before GPU initialization; the test INI was restored byte-for-byte.
- A completed DX12 debug-layer + GPU-based-validation run passed capture depth/overlay, atomic publication, static cache reuse, material/sky/probe invalidation, classification, camera cut, world/probe identity replacement, mode/resolution/quality switches, box projection, overlap normalization, roughness mips, opaque/transparent PBR, HDR composition, XeSS reset/odd resize and final tone mapping.
- Readback oracles: static red radiance 4.08984 hid the dynamic green surface behind it; moving green in front produced 4.11719; its roughest mip fell to 2.73242. An off-camera sun caster reduced the receiver from 4.51953 to 0.0906372. No D3D12 error/corruption message was accepted.
- The connected ECS/material regression passed with 1/4 job threads: static wall versus flying-prop classification and real material DirtyFlag upload. A subsequent regression assertion now also covers adding a runtime probe entity without erasing previously uploaded transforms.
- The unchanged Bistro scene loaded and rendered its existing one-frame loading benchmark through HDR XeSS and the final screen pass, exit 0. Initialization 40.5651 s, ready 41.0442 s, 309 prepared textures. This is a startup/render smoke, not a visual acceptance or steady-state performance measurement.
- Task-owned `git diff --check` passed. No probe, entity, material, light or demo content was added to Bistro.

## GPU measurements already completed

RTX 3060 Laptop GPU, one probe, three small quads, static steady-state overlay, sun disabled, 24 measured frames per preset, validation disabled. GPU timestamps cover reflection capture/GGX/publication, excluding CPU/readback, environment preparation, main local shadows and presentation. These measurements precede the final fixture tightening to single-sided geometry; they are not Bistro timings.

| Preset | Face pixels | Faces/frame | GGX samples | Mean GPU ms/frame | Seven reflection textures |
| --- | --- | --- | --- | --- | --- |
| Low | 64 | 1 | 32 | 0.0251307 | 5.25 MiB |
| Mid | 128 | 2 | 64 | 0.0756053 | 8.375 MiB |
| High | 256 | 6 | 128 | 0.748501 | 21.125 MiB |

Texture allocation excludes descriptor heaps, frame buffers and the separate static local-shadow maps. Low/Mid amortize one complete GGX publication across six/three frames respectively.

## Current handoff: implementation only, not verified

The latest user instruction stops verification in this chat. Own queued Reflection/ECS validation processes were stopped; no build, test, GPU fixture, benchmark, visual validation or separate review/cleanup pass is being run for the subsequent changes. The main chat owns verification.

The follow-up moves Implementation, Resolution, FacesPerFrame, PrefilterSamples and UpdateInterval into each ReflectionProbeComponent. Only Enabled and Quality remain global CVar/INI settings, as explicitly clarified by the user in this chat. The capture pass resolves author/preset values per probe, allocates matching baked color/depth per probe, uses matching mip subresources of shared scratch capture resources for the scheduled probe and publishes lower-resolution captures into a matching mip range of the shared cube array. Shader metadata includes MinMip as well as MaxMip. Existing fixture call sites were adapted without adding new checks.

These ownership/resource changes are unverified. The completed GPU results above apply to the earlier global-parameter implementation, not to this follow-up. Runtime buffer-growth assertions and the tightened single-sided/queued-frame fixture were also queued but not completed before the stop instruction.

The old request to coordinate GPU mutex release is no longer needed. No other chat's process or marker was modified.

## Integration and limits

See [reflection-probes.md](reflection-probes.md) for the two global CVars, per-component fields, defaults/ranges, authoring, exact SSSR radiance fallback seam and separate common HDR integration. The implementation uses one shared working capture and GGX scratch cube plus a published cube array; complete copy publication avoids a per-probe double-buffer framework.

The baked mode deliberately retains static-only shadows on baked receivers; full dynamic mode is required for moving caster shadows on them. Transparent surfaces consume probes but are excluded from capture; glass-in-glass and multi-layer refraction are not represented. Main-camera navigation, long-run stability and visual probe placement in Bistro remain unverified; no probes were added there for demonstration. Multi-GPU is not implemented or tested.

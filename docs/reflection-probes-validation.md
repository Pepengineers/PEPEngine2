# Reflection validation — 2026-11-01

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

## Final reruns waiting for the shared GPU slot

After the completed full run, the connected-code review fixed runtime upload-buffer growth and tightened fixture geometry to one-sided triangles. The final fixture also queues multiple frame slots without a Flush after every frame, then switches resolution/mode while work is pending. These final GPU/ECS reruns have been built and queued, but have not yet executed: the shared mutex is held by the Motion Blur visual-validation shell (PID 21016). Its holding command waits for its own `Build/MotionBlur/bistro-validation-hold` marker; that chat is waiting on approval. No other chat's process or marker was changed.

The final acceptance record must be updated after those queued checks and the frame-slot profiles complete. Earlier completed GPU results must not be presented as execution of the newly tightened assertions.

## Integration and limits

See [reflection-probes.md](reflection-probes.md) for the seven CVars, defaults/ranges, authoring, exact SSSR radiance fallback seam and separate common HDR integration. The implementation uses one shared working capture and GGX scratch cube plus a published cube array; complete copy publication avoids a per-probe double-buffer framework.

The baked mode deliberately retains static-only shadows on baked receivers; full dynamic mode is required for moving caster shadows on them. Transparent surfaces consume probes but are excluded from capture; glass-in-glass and multi-layer refraction are not represented. Main-camera navigation, long-run stability and visual probe placement in Bistro remain unverified; no probes were added there for demonstration. Multi-GPU is not implemented or tested.

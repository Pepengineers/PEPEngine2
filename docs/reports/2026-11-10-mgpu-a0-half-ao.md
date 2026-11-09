# Half-resolution ambient occlusion — 2026-11-10

`Render.AO.Resolution` is a runtime enum. Full (0) is the default. Half (1)
uses `ceil(sceneWidth/2) × ceil(sceneHeight/2)` independently of scene render
scale, AO quality and placement. Lighting receives full-resolution R32_FLOAT
visibility in both modes.

Half picks the largest reverse-Z depth in each clipped 2×2 tile and preserves
that pixel's normal and offset. The odd-size projection extends the viewport
without changing scene pixel pitch or jitter. Upsampling compares the receiver
normal and its view-space tangent plane with each candidate. Background and
pixels with no matching sample receive visibility 1. A single working pixel
also receives 1. XeGTAO pads the prefilter to whole mip tiles. Tiny CACAO inputs
are edge-padded to satisfy the vendor mip chain and cropped on resolve. The
native CACAO downsample flag stays off.

Half textures are allocated on first active use. Full and Half retain separate
descriptors and native CACAO contexts across queued mode changes. Resize
recreates allocated grids after the renderer's queue drain. Changing resolution
or resizing requests an upscaler history reset. AO has no history of its own.

## Verification

Base `97f4fac4e4c4767c2495a0544c3cbce506780857`. The following runner states and
their stdout/stderr files are in the A0 worktree's `Build/A0`. All four returned
exit 0 with empty stderr.

| Check | Final state |
| --- | --- |
| AOTests Debug/x64, VS 18 Professional v145, local P0 /Z7 targets | `build-ao-tests-debug-final.json` |
| CVar defaults, enums, rejection, INI and quality independence | `ao-cvar-cpu-final.json` |
| Debug Layer with `--ao-only` | `ao-debug-layer-seeded.json` |
| GPU-Based Validation with `--gpu-validation` | `ao-gbv-seeded.json` |

Both GPU runs verified Direct/Compute queue selection and exact float readback
parity on the main fixtures for all four algorithms. The Half working textures
were 33×25 for a 65×49 scene. The fixture compared all reduced depth texels
exactly against the maximum of each original tile, verified the winning normal
and offset, and checked that the unmatched surface and the trailing background
column remained exactly neutral. The fixture also covered jitter, camera-cut
history flags, disabled output, queued mode switches and resize to 17×13,
3×3, 1×17, 1×1, 321×241 and 1280×720. Queued frame samples matched the isolated
reference within the test's `1e-5` tolerance. No final numerical comparisons failed.

The mean visibility reduction relative to the jittered Half plane near the
depth step was 0.113567 (SSAO), 0.121426 (HBAO), 0.0933354 (XeGTAO) and
0.156585 (CACAO). These are fixture correctness values. The nine
`ao-half-*.pgm` readbacks in `ao-debug-layer-seeded-images` and
`ao-gbv-seeded-images` matched byte-for-byte. The contact sheet
`ao-half-step-readbacks.png` places 33×25 working output on the left and
65×49 lighting output on the right, with rows in algorithm order. XeGTAO
also shows its wider angular shading on the plane.

The first build encountered a Git LFS pointer for the Wwise library. The
first test launch found LFS pointers in the local runtime DLLs. Validation
used existing local bytes verified against those pointers' SHA-256 values.
The runtime copies are listed in `runtime-lfs-hydration.json`. The tracked
Wwise path was restored to its original pointer after testing. The first
Debug Layer run also exposed a fixture assertion that treated 1×17 as
1×1. The corrected run passed, and its runner initially censored the result
because the engine saves its runtime CVars on shutdown. Both final GPU
jobs verified the working INI against immutable `Engine.seed.ini` at start
and saved the resulting INI separately.

The executable SHA-256 was
`358b87d146c23d33c912f1e671d97f9eb5a759cdd52b07d61dd1de640056d63a`.
The runner states record input hashes. The process receipt
`processes-final.json` recorded zero remaining packet workloads.

The isolated fixture exercised both placement settings using synthetic
bindings. Real early/late G-buffer bindings belong to the integrator's
Bistro placement matrix. These runs covered one selected adapter and
focused on AO. Bistro, vendor XeSS, performance and preset tuning were
outside the validation slot.

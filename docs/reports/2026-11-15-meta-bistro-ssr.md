# Bistro FidelityFX SSSR without reflection probes

## Implementation

The ordinary path now runs deferred opaque lighting, existing SSSR prepare,
FidelityFX SSSR + reflections denoiser, specular resolve, existing All mesh WBOIT,
WBOIT resolve and HDR display/PRESENT. No full SceneWaterParticles,
SceneReflections, probe entities, capture pipeline, scene snapshot or diagnostic
readback tail is enabled. Shadow cache/sparse uploads, AO and materials are unchanged.

`GDX12SSSRPass::SetStaticIBLOnly(true)` selects a minimal shader variant without
ReflectionProbes.hlsli/SampleReflectionRadiance; ordinary bindings and accesses
omit probe SRVs t9/t10. The default App pass keeps its probe-enabled variant.
Fallback samples the same prefiltered environment, roughness*7 mip and intensity
as deferred PBR. SDK reflection/fallback blending is retained; resolve adds only
(radiance - fallback) times the split-sum BRDF, once. Denoiser alpha is not used
as a blend mask. Existing baked, rough/nonreflective and sky rules are retained.

The inputs are opaque HDR, opaque reverse-Z depth, world normals/roughness,
F0/baked flag, GBuffer motion, prefiltered environment and BRDF LUT. GBuffer
attachment 1 already writes prevNDC-currentNDC using previous/current unjittered
camera transforms. Active-camera constants update every frame even with paused
simulation; static vertex PreviousPosition is initialized to Position. Existing
motion conversion to UV (0.5,-0.5), jitter delta, normal unpack multiplier 1 and
squared roughness are unchanged.

## History, queues and resources

Ordinary SSR uses `RenderPipelineCommonData::HistoryRevision` from the camera
update system, not probe history. This revision changes on camera/history reset,
not ordinary camera movement. GBuffer addresses rotating across three frame slots
are not compared as discontinuities. One persistent SDK context belongs to the
ordinary presented sequence. Resize, settings, camera/history revision,
environment revision and disable/re-enable reset the context/history as needed.

SDK/private history packets run on DIRECT in submission order, with prepare and
resolve also recorded through Meta native work. Their imported accesses/states
match SDK entry and restored states; resource hazards order prepare/SDK/resolve
and the WBOIT consumer. The first real dispatch retains `OwnerRecording` for the
backend's lazy constant-ring allocation. Subsequent dispatches reuse the ring.

The bundled `ffx_types.h` sets `FFX_MAX_QUEUED_FRAMES=4`. The configured three
frame slots fit this contract; ordinary construction rejects a larger unsupported
slot count instead of inserting per-frame Flush. Existing context destruction
drains the DIRECT queue before replacing SDK history, and pipeline resize drains
the executor before changing scene resources. These are discontinuity/lifetime
waits, not ordinary-frame waits. Camera constants on UPLOAD retain GENERIC_READ.

`Render.SSR.Enabled=false` returns before SSR allocation/dispatch/copy-through,
passes the existing opaque HDR to WBOIT and marks history for reset on re-enable.
The retained diagnostic path keeps its existing copy-through and probe behavior.

## Effective startup settings

SSR Enabled=true, Quality=2 (Mid). Existing inherited defaults resolve to:

- MaxIntersections=128, MinOccupancy=4, MostDetailedMip=1.
- SamplesPerQuad=2, VarianceGuided=true, VarianceThreshold=0.05.
- RoughnessCutoff=0.7 perceptual (SDK squared threshold 0.49).
- Thickness=8 cm, TemporalStability=0.9, iblFactor=1.

No new SSR controls or algorithm were added. Native resolution, primary adapter 0,
paused simulation, free camera, source interior/exterior, lights, shadow/AO
parameters and material adjustments from the preceding stage are preserved.

## Viewing and limitations

Inspect the glossy metal bar/counter inside Bistro and other existing reflective
opaque surfaces while moving the camera. Probes are deliberately absent: misses
use static IBL, and reflections of objects behind the camera are not promised.
Only opaque HDR/depth/GBuffer data participates. Transparent glass receives its
existing forward static IBL; glass reflections and transparent surfaces captured
in SSR are not implemented. Water, particles, grass, clouds, physics/animation,
refraction, other post effects, reconstruction and FG remain excluded.

## Verification

Release x64 build passed through the Visual Studio Developer Prompt outside the
sandbox, retaining the existing external-library LNK4099 missing-PDB warnings.
One ordinary no-argument launch executes prepare, SSSR + denoiser and specular
resolve before the two existing mesh WBOIT packets and HDR display. Current-run
logs contain no ERROR/FATAL. Entrance/terrace rendering and transparent glass
remain visible after camera movement. The user confirmed the requested close
visual reflection/camera check with "все окей"; that observation is accepted
without a repeated run. The log subsequently records normal resource shutdown
and release. No --smoke, new tests, matrices, captures, profiling or long-run
harness was used.
Existing `SSRTests.cpp` and `SSRBistroTests.cpp` were located for source context;
the current verification contract does not require running them.
Disabled/reset/resize paths are reviewed in source, without extra runtime modes.
Independent source review and the simplification pass found no actionable issues;
`git diff --check` passed.
No performance cost or speedup is claimed. Image/FPS/CPU/GPU acceptance belongs
to the user; probes and the next technique remain deferred.

Build/run logs and operation dumps: `Build/Verification/meta-bistro-ssr/`.
Executable: `D:/Project/PEPEngine2/Build/bin/x64/Release/App.MetaRenderGraph/App.MetaRenderGraph.exe`.
Pre-existing SceneManagerModule.cpp/BistroAdditionsTests.cpp edits and Temp/ are
preserved and excluded from this task's commit.

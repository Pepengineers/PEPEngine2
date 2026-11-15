# Bistro Motion Blur and reconstruction

## Review fixes

Separate local commit `c94a5083` removes ordinary Image/Batch/Scene READBACK
allocations, diagnostic offsets and unused readback resource bindings. The final
graph remaps only resource accesses after all techniques append; native operation
IDs and diagnostic readback paths remain intact. It also reuses immutable screen
font snapshots by ImGui texture identity/UniqueID, dimensions and real atlas dirty
generation; GPU reuse compares snapshot identity, without full pixel comparisons.
Old generations remain owned by draw/recording data through retirement. Generic
CaptureDrawData/default offscreen discard semantics are unchanged. This follows
the pinned ImGui TexIsBuilt dirty contract; no vendor code is changed.

## Post chain and settings

Ordinary Meta keeps its original scene pipeline and now appends:
opaque + SSSR → mesh/particle transparency/additive → Motion Blur/complete motion
→ reconstruction/native → HDR display → screen UI → PRESENT.
No DoF, probes, water/physics, clouds, posters, FG or offload is enabled.
Actor counts (10 people/5 foxes), particle/grass budgets and neighbouring quality
are preserved, including accepted grass culling without bend and shadow caches.

Motion Blur explicitly uses App source defaults: Enabled=true, Quality=2 (Mid),
ShutterFraction=0.5, MaxRadius=40 output pixels, Samples=0 (27 taps by quality).
The existing Prepare/Tiles/Neighbors/Gather shader and all other controls remain.
Velocity is previous-minus-current unjittered NDC, reverse-Z depth; Prepare fills
sky camera motion and feature coverage. Radius uses output pixels and scales to
the render grid. Particle feature motion comes from the currently prepared pass
output, with actual weighted-raster write → post read dependencies. Particle color
is not composited twice. The explicit helper input replaces temporary global
motion pointer swapping in the diagnostic path too.

The ordinary MotionBlur helper opt-in aliases input HDR when gather is off, and
prepares complete motion for temporal SR independently of blur enable. Native/NIS
without blur skip Prepare, tiles/gather, particle motion and full-screen copies.
Targets allocate lazily; runtime tile changes require the owner drain before
recording. Default App helper output/copy semantics are retained.

## Reconstruction, lifetime and history

Startup requests XeSS/Default, mapped by existing CreateProvider to Ultra Quality
Plus. Native, XeSS, FSR, DLSS and NIS retain their existing SDK/preset controls and
explicit fallback. Native has no standalone TAA, NIS is spatial; XeSS AA, FSR
NativeAA and DLSS DLAA provide native-resolution temporal AA. No new SDK or TAA
algorithm is introduced. The UI reports requested/effective provider, requested
quality, fallback reason and render/output dimensions; effective startup result
is recorded with verification below.

Reconstruction Setup/Query determines the grid before geometry and CameraCB.
Bindings include blur/reconstruction/output but keep FG inputs disabled. Settings
changes drain the executor before replacing SDK state and invoke existing resize;
window resize re-queries render dimensions before targets rebuild. Temporal
provider flags select camera jitter; Native/NIS leave it off. The same provider
context survives adjacent slots and receives current SetInputs every frame.

DIRECT owner-recorded boundaries declare real resources/states and alias-safe
accesses. Current/previous particle, grass, pose and transform histories retain
their existing adjacent-frame contracts. Reset/cut/resize/re-enable bypass gather
and reset temporal history. Native output links refresh every frame across HDR/
blur aliases, and reset flags retire after consumers even without an SDK provider.
UI remains window-resolution, sharp and after reconstruction/display. Font data,
static geometry and SDK contexts are not re-uploaded/recreated every frame.

## Verification and limits

Release x64 build passed (exit 0, existing Jolt.pdb/LNK4099 warnings). One ordinary
no-argument launch presents the Bistro, actors, particles and sharp Engine.UI.
The status window confirms requested XeSS/SDK default, effective XeSS without
fallback, render 985×616 and output 1280×800. The existing default maps to Ultra
Quality Plus. In that same run the user confirmed camera-motion blur and sharp UI:
"Да, всё работает". The current-run log has no ERROR/FATAL, only the existing
large OBJ import-budget warnings. Saved steady operations contain motion blur,
reconstruction, display, one UI composition and PRESENT, without FG or DoF.
The first compile
found a C++ comparison incompatibility in the new settings check; it was corrected
to the existing equality operator before rebuilding. No successful smoke will be
repeated. Existing MotionBlur, reconstruction/frame-generation and offscreen UI
tests were inspected as source context, but no tests were added or run, per task
and AGENTS. No provider/preset matrix, accuracy check, stress, profiling or capture.
No FPS/CPU/GPU improvement is claimed. Other providers/presets and fallback paths
remain unverified at runtime. Local reflection probes are not started.
Independent correctness, performance, standards, testing and maintainability
source review found no actionable findings. A separate simplification pass found
no necessary changes; no verification scope was expanded.

Artifacts: Build/Verification/meta-bistro-motion-blur-upscalers/.
Executable: Build/bin/x64/Release/App.MetaRenderGraph/App.MetaRenderGraph.exe.
Pre-existing SceneManagerModule, BistroAdditionsTests, particle optimization and
Temp changes remain outside the task commits; shared files isolate task changes.

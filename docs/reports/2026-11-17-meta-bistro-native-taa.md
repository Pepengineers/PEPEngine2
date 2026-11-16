# Bistro Native TAA — 2026-11-17

## Scope and controls

Added `GDX12UpscalerProvider::TAA=5` (UI: Native TAA); 0..4 retain their numbers.
Native 0 remains passthrough. TAA Default/NativeAA both render at output resolution;
other presets explicitly fall back to Native with the existing requested/effective
status and reason. Ordinary Meta Startup selects provider 5, quality 0; shared App
still defaults to XeSS. No second temporal pass over SDK output, sharpening, FG,
TAAU, dependencies or queue/GPU redistribution.

`Render.TAA.HistoryWeight=0.9` is the maximum history contribution, finite [0,1).
`Render.TAA.VarianceGamma=1.0` is a finite positive standard-deviation multiplier.
The existing CVar validator rejects invalid assignments, preserving the prior value.
Both are live parameters without resource replacement; they are initial values,
not quality tuning. `Render.Reconstruction.Restart` recreates the provider through
existing retirement. Provider/preset changes and resize follow that same path.

## Motion, depth and resolve

Post order: scene composition (including water and preview spheres) → DoF →
complete motion / optional Motion Blur → TAA → HDR display transform → screen UI
→ PRESENT. Meta supplies the final composition depth/motion, not raw opaque depth.
The existing jitter flag requests complete motion even with blur disabled, and
the particle consumer sees the flag before current-frame preparation.

`GPUDataUpdateSystem::_jitterFrameIndex` remains the sole camera Halton (2,3)
sequence. RH raster projection uses `proj._31 -= 2*jx/width`,
`proj._32 += 2*jy/height`: stored jitter is positive raster displacement in pixels.
Geometry/complete motion is unjittered `previousNDC-currentNDC`, matching
`MotionBlur.hlsl`. The final resolve follows Microsoft's sample: take the current
raster sample `Color[p]` and accumulate the different subpixel samples at one
fixed nominal output pixel. Accumulated color uses
`previousUV = outputUV + motion*(0.5,-0.5)`, with no previous-current jitter delta.
The minus on motion Y converts upward NDC Y to downward texture Y. Raw depth
history still belongs to its raster grid, so its lookup alone uses
`depthUV = previousUV + (previousJitter-currentJitter)/size`, matching MiniEngine's
separation of temporal-color and depth coordinates. No second sequence or stream
keyed by frame slot is introduced. Depth/motion use raster pixel p and world
reconstruction uses its raster center and jittered inverse matrix.

Reverse-Z depth reconstructs current world position through the jittered
`Camera.InvViewProj`. Positive current view distance is stored in R32_FLOAT history;
the submitted previous unjittered view-projection transforms current world position
to expected previous distance (`clip.w`). Reject mismatch above max(0.01 world unit,
2% of expected distance). This accounts for camera translation/rotation rather
than comparing raw depths from different cameras. Object motion along view Z has
no dedicated previous-depth velocity; conservative rejection can lose accumulation.
Zero reverse-Z depth has a separate sky path: complete motion removes camera
translation and only a previous sky sample (history depth zero) is accepted.

Color history uses bilinear reconstruction; current color uses the sample's
point-loaded raster sample. Depth validation accepts a matching finite sample
from the previous 2×2 gather footprint, so subpixel motion across a depth edge
does not unconditionally discard history. Color reprojection accepts only
in-bounds pixel-center UVs and finite data. Invalid/reset history returns
current color and initializes new color/depth without sampling old texels.
Clip history RGB to the current 3×3 mean ± gamma·standard deviation, following
the sample's variance-clipping mode. Moments are scaled to keep HDR arithmetic
finite; the shader caps the multiplier at 65504 to avoid overflow for extreme
finite CVars. Output RGBA16_FLOAT RGB is finite, nonnegative and ≤65504; current
alpha is retained after finite [0,1] sanitization. HDR blending follows MiniEngine:
`ITM(lerp(TM(current), TM(clippedHistory), weight))`, where
`TM(c)=c/(1+luminance(c))` and ITM is its inverse. This is an internal reversible
blend-space transform, not the display transform. Base weight decreases with
motion (sample speed limit 64 pixels) and history displacement caused by clipping.
Comparing raw history against a noisy current subpixel sample was removed: that
discarded the accumulation precisely when it was needed to suppress flicker.

Final algorithm sources, read as actual code after the user rejected flicker:

- [Microsoft GDK TemporalResolve.hlsl](https://github.com/microsoft/Xbox-GDK-Samples/blob/73f8aa373138ecd6331a05ac908a88c287602db8/Samples/Graphics/TemporalAntialiasing/Shaders/TemporalResolve.hlsl):
  fixed-pixel accumulation, 3×3 moments, variance clipping, bilinear history and
  0.9 accumulation convention. GeometryPassPS/VS were also checked for motion/jitter.
- [MiniEngine TemporalBlendCS.hlsl](https://github.com/microsoft/DirectX-Graphics-Samples/blob/master/MiniEngine/Core/Shaders/TemporalBlendCS.hlsl):
  separate depth jitter offset, depth gather, speed rejection and HDR blend.
- [MiniEngine ShaderUtility.hlsli](https://github.com/microsoft/DirectX-Graphics-Samples/blob/master/MiniEngine/Core/Shaders/ShaderUtility.hlsli):
  luminance-preserving TM/ITM pair.

Shader retains Microsoft copyright/MIT permission. No sample host, GDK resource
ownership, sharpen, separate jitter sequence or additional SDK was imported.
Local adaptations are the existing NDC motion format, reverse-Z/camera-correct
distance validation, finite HDR handling and color-change rejection required by
the scene's animated/transparent materials.

## Ownership and graph contract

One persistent pair of RGBA16_FLOAT color and R32_FLOAT depth images belongs to
the viewport. The written color is also display input, with no history copy.
Owner preparation fixes read/write images, current input links, camera CB address,
root constants, current jitter and matrix before output publication. Meta captures this
frame by value; recording only binds descriptors and records barriers/dispatch.
Root constants are embedded in the command list. Camera upload memory follows
the existing frame-slot fence; no descriptors are rewritten in steady state.

History advances once after successful submission of the frame. A failed or
partially submitted Meta execution invalidates pending history and uses existing
executor abort/retirement. Reset flags are consumed after successful submission,
not while building the graph. First frame, camera cut/world reload, resize,
provider/preset/restart and common history revision/reset all reject old history.
Object/camera movement, routine probe publication and sun progression alone do
not explicitly reset TAA.

Meta declares current HDR/depth/complete motion, both history reads, both history
writes and camera constants. The boundary uses ALL_SHADER_RESOURCE (camera upload:
GENERIC_READ); recording transitions inputs to NON_PIXEL_SHADER_RESOURCE and
outputs to UAV then SRV, and its state tracker restores declared layouts. Graph
terminal transitions leave persistent images in COMMON. All TAA and display
packets run on primary DIRECT, so earlier display readers finish before frame+2
overwrites a ping-pong image, including with three frame slots. Resources live
until existing resize/settings retirement drains their users.

The shared App imports both physical history pairs into its graph, declares their
read/write use and camera buffer access, prepares bindings before graph execution,
and commits/cancels history around its existing DIRECT submission. Recording and
shader are shared with Meta. Native/NIS retain jitter-free behavior and existing
bypass/SDK paths. Minimized/zero-sized preparation disables dispatch.

## Verification

Source review covers Native bypass, TAA with blur disabled, reset/resize, particle
consumer timing, composition inputs and shared App caller. Existing reconstruction
tests were inspected; the invalid enum sentinel was adjusted from 5 to 6 without
adding scenarios. Automated tests and mode matrices are deliberately not run or
expanded, per task/AGENTS Verification Contract.

Release builds of Meta and App passed before the final reference port. First ordinary launch showed effective
Native TAA, 1280×800 render/output, visible scene and sharp screen UI, and the
steady graph included the GPU resolve between motion and display. The user
reported severe jitter, so that launch **failed visual acceptance**. An intermediate
current-color resampling correction was also rejected by the user for whole-scene
flicker at a stationary camera. Neither run counts as accepted visual quality.
The final resolve was then replaced using the official implementations above.
DXC `cs_6_8`, HLSL 2021 compilation passed for the final shader.

Source audit found no unconditional per-frame reset: AO initializes once per slot;
SSR compares fields, DoF uses unjittered projection, continuous sun changes preserve
revision, and ordinary probe rebakes do not invalidate TAA. No dynamic reset-flag
evidence was collected; do not treat that audit as proof of runtime validity.

The user stopped Computer Use with physical Escape before a final-reference
visual run. No further application inputs/launches were issued. Final flicker,
camera movement/stop and ghosting acceptance remain **NOT VERIFIED**.
Final Release rebuilds of both App.MetaRenderGraph and App passed after the
reference port and removal of temporary diagnostic logging. Logs remain in
`Temp/native-taa-build-complete.log` and `Temp/native-taa-shader.log` (not committed).
`git diff --check` passed. Final source review found no arithmetic/lifetime blocker;
it does not replace the outstanding visual acceptance.
Initial direct MSBuild attempt failed with MSVC C1902 (PDB manager environment);
the build through VS `vcvars64.bat` succeeded, with existing external conversion
and missing Jolt PDB warnings.

## Limits

Mesh transparency retains the existing opaque/composition velocity/depth contract;
it has no full independent transparent-surface velocity/depth. Particle coverage
and sky motion reuse the existing complete-motion preparation. World-space UI and
cloud pictures have geometric velocity but no texture-animation velocity; color
rejection reduces trails without proving their absence. DoF/blur inputs already
mix surfaces. Accepting matching depth from a gather footprint can mix edge
history. Bilinear history can soften detail; no sharpen is added. A short
smoke cannot establish ghost-free output for every material. Native-resolution
inputs differ from former XeSS dimensions, so this stage is not a performance A/B.

Stop at user assessment of image/FPS/CPU/GPU; no following stage is started.

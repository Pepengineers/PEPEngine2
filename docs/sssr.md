# AMD FidelityFX SSSR and linear HDR resolve

The single-GPU pipeline is: opaque G-buffer → deferred linear HDR lighting →
AMD SSSR + Reflections Denoiser + specular resolve → forward WBOIT → linear HDR
composition → HDR XeSS → Reinhard/sRGB display output. SSSR runs at render
resolution, before XeSS: depth, normals, radiance and motion share the same pixel
grid, and reflections receive temporal upscaling with the scene.

SSSR consumes reverse-Z RH depth, signed world normals, squared roughness prepared from perceptual roughness,
unjittered motion (`previousNDC-currentNDC`), a GGX-prefiltered 128px/eight-mip
environment and the existing DFG LUT. Baked/unlit surfaces and background do not
spawn rays. Only the SDK's private motion input converts NDC to UV and adds
`(currentJitter-previousJitter)/renderSize`. Previous view-projection is the
actual previous jittered matrix, including camera motion. Matrices are copied
from the engine's row-vector row-major representation to the equivalent SDK
column-vector column-major representation, without an extra transpose.

The SDK output RGB is reflected **radiance**, already blending ray hits and
global environment on misses. Raw trace alpha stores hit-ray length; the denoiser
reuses alpha as scratch payload. Final alpha is ignored, never confidence. Resolve
computes `litHDR + (SSSRradiance - oldGlobalRadiance) * (F0 * DFG.x + DFG.y)` on
eligible opaque pixels. Thus the existing direct/diffuse/emissive contributions
stay intact, and indirect specular is replaced once. Excluded roughness/baked/sky
pixels stay unchanged. Off is an exact GPU copy. No LDR texture feeds SSSR or XeSS.

Transparent bottles retain forward PBR/global IBL and WBOIT; they do not write the
opaque G-buffer and are neither SSR receivers nor ray-hit geometry. No Bistro
objects, materials or scene fixtures are added for SSR.

The concrete local-probe integration seam is `SSSR.hlsl::Resolve`'s old-radiance
term, paired with the same indirect-specular term in `PBRLighting.hlsli`.
A future probe branch must supply the **same** fallback to lighting and resolve,
and choose how offscreen ray misses sample local probes. The SDK accepts one
global cubemap; replacing only deferred IBL with probes would leave the wrong
subtraction/fallback. There is intentionally no unused probe interface here.

SDK 1.1.4 exposes no history-reset dispatch flag. Context recreation resets SSSR
and denoiser together after waiting for prior GPU work, on cut, resize, re-enable
and input-resource identity changes. `CameraComponent::InvalidateHistory()` marks
a teleport/cut. Camera switch and projection change also increment the shared
`RenderPipelineCommonData::HistoryRevision`; XeSS consumes that revision and
resets on first dispatch/resize. Continuous camera/object movement retains history.
Quality switches keep compatible history; roughness changes are rejected using
the denoiser's roughness/depth/normal tests. Command-list binding caches are
invalidated after each external SDK dispatch; the AMD backend restores imported
resource states before returning. Shared inputs retain combined pixel/compute SRV
states for subsequent forward WBOIT. Private motion/roughness/radiance stay in compute-read.

The pinned SDK needs a one-binding correction (`External/FidelityFX/cold-history.patch`):
denoiser prefilter and temporal resolve read this frame's average radiance. Reading
the previous ping-pong average clips valid first-frame reflections to black.

## Runtime controls

Each CVar is registered once in `GDX12SSRSettings.cpp`, before INI loading, with
`Default` flags. Change it with `ConsoleModule::FindConsoleVariable(...).SetFromString(...)`
or `Config/Engine.ini` beside the exe. Invalid enums, ranges and non-finite values
are rejected without changing the current value. A per-frame value snapshot is
used by both dispatch and resolve.

| CVar prefix `Render.SSR.` | Default | Range / units |
|---|---:|---|
| Enabled | true | bool; false bypasses SSSR and denoiser |
| Quality | 2 | 0 Off, 1 Low, 2 Mid, 3 High |
| MaxIntersections | -1 | -1 preset; 1..1024 depth-hierarchy lookups/ray |
| MinOccupancy | -1 | -1 preset; 0..32 active lanes; 0 disables early exit |
| MostDetailedMip | -1 | -1 preset; 0..6; further limited by image size |
| SamplesPerQuad | -1 | -1 preset; 1, 2 or 4 rays per 2×2 quad |
| VarianceGuided | true | bool; allow additional rays on temporal variance |
| RoughnessCutoff | -1 | -1 preset; 0..1 perceptual roughness; squared for SDK |
| Thickness | 8 | (0,1000] scene centimetres, hit tolerance |
| TemporalStability | -1 | -1 preset; 0..1, denoiser history stability |
| VarianceThreshold | 0.05 | 0..1 luminance variance threshold |

| Preset | MaxIntersections | MinOccupancy | MostDetailedMip | SamplesPerQuad | RoughnessCutoff | TemporalStability |
|---|---:|---:|---:|---:|---:|---:|
| Low | 64 | 8 | 2 | 1 | 0.5 | 0.8 |
| Mid | 128 | 4 | 1 | 2 | 0.7 | 0.9 |
| High | 256 | 0 | 0 | 4 | 0.9 | 0.95 |

Explicit overrides survive preset changes. Set an override back to `-1` to
inherit. No implementation selector exists because only AMD SSSR is implemented.

## Verification

Build `Apps/App/App.vcxproj` and `Apps/App/SSRTests.vcxproj` with Release/x64,
`/p:PreferredToolArchitecture=x64 /m:1 /nr:false`. The documented host override
`$env:_CL_='/Z7 /GL-'` avoids sandbox C1902 without changing the global toolchain.
Run `Build/bin/x64/Release/SSRTests/SSRTests.exe --cpu-only` for CVar checks, or
without arguments for GPU readback under the debug layer. GPU validation owns
`Local\PEPEngine2FeatureGpuValidation` for the entire process.

The analytic fixture tests distinct red/green HDR ray hits, single-specular energy,
miss fallback, exact Off, roughness exclusion, warm/cold history, cut, resize,
re-enable, odd dimensions, frame slots, stationary jitter and camera/object motion.
It is separate from Bistro visual validation.

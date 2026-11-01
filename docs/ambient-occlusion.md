# Ambient occlusion

`GDX12AmbientOcclusionPass` consumes the opaque G-buffer's reverse-Z device depth
and signed world-space shading normals. It outputs diffuse ambient visibility in
`R32_FLOAT`, at the same jittered render resolution, in [0,1]. One means neutral.
It runs before deferred lighting and XeSS: the existing G-buffer can be used
directly, and the upscaler also accumulates AO detail. It adds no scene objects.

Deferred lighting binds the output at `t12` and multiplies **only diffuse IBL**.
Direct sun/local lights, emissive, baked color and specular IBL are unchanged.
Scalar screen-space AO is not used as a substitute for directional specular
visibility. Forward transparency uses neutral visibility. No second AO multiply
occurs in composition.

The libraries use positive-Z view positions. Preparation transforms world normals
with the camera's RH view, negates view Z, then encodes [-1,1] to [0,1] for CACAO.
XeGTAO decodes those normals. CVar radii, bias and fade are scene centimetres.
SSAO/XeGTAO use centimetres;
CACAO converts its projection depth term, radius and fade to metres to keep its
native FP16 linear-depth chain in range. Author fields and device depth are unchanged.
Projection row 2 jitter offsets are included in both libraries' reconstruction.

SSAO projects cosine-weighted hemisphere samples into device depth, with a depth
bias and radius falloff. HBAO marches screen-space azimuths and integrates the
increase of the normal-relative horizon sine, weighted by squared world distance;
it is a distinct horizon algorithm, not SSAO with another sample count. Their
denoiser alternates horizontal/vertical bilateral passes with depth and normal
edge weights. Intel XeGTAO and AMD FidelityFX CACAO use their official algorithms;
the pinned sources and local adaptations are documented alongside the libraries.

AO has no temporal history. Every dispatch writes its complete logical output,
including odd dimensions and background. The output is explicitly filled with
one when disabled, Quality=Off or Intensity=0. Viewports with both dimensions below
16 also use neutral AO because the native fixed depth mip chain requires a
minimum size. Resize requires the renderer's normal GPU drain. The native CACAO
context is created on first use and retained across algorithm/quality switches;
its size-dependent resources are recreated on resize. It may cause a CPU hitch
on first selection. AO constants have one upload slot per engine frame slot;
CACAO's upload ring uses that same configured frame count.

All settings below are `Default` CVar, registered once in a `.cpp` before INI
loading, and can be changed through `ConsoleModule::FindConsoleVariable(...).
SetFromString(...)` during runtime. Values outside their finite ranges are
rejected without modifying the old value. Sampling/denoise overrides default to
`-1` (inherit); changing Quality never changes the stored overrides. Resetting an
override to `-1` resumes preset control. Quality is independent of Implementation.
The renderer takes a validated copy of settings each frame; fade end is resolved
to at least fade start + 1 cm without rewriting either CVar.

All names in this table have prefix `Render.AO.`.

| Name | Default | Units / accepted range |
| --- | ---: | --- |
| Enabled | 1 | 0/1 |
| Quality | 2 | 0 Off, 1 Low, 2 Mid, 3 High |
| Implementation | 2 | 0 SSAO, 1 HBAO, 2 Intel XeGTAO, 3 AMD CACAO |
| RadiusCm | 50 | cm, 0.1..10000 |
| Intensity | 1 | 0..5; zero is neutral; CACAO native shadowMultiplier |
| DenoisePasses | -1 | -1 inherit, 0..8; library-specific edge-aware filter |
| Sharpness | 0.98 | 0..1 edge preservation; XeGTAO denoise beta = 0.2 + value |
| FadeStartCm | 5000 | cm, 0..1000000 |
| FadeEndCm | 15000 | cm, 1..1000001; resolved >= start+1 |
| HorizonBias | 0.06 | normal-relative horizon sine, 0..0.2; HBAO/CACAO |
| SSAO.Samples | -1 | -1 inherit, 4..64 hemisphere samples |
| SSAO.BiasCm | 0.5 | cm, 0..100 |
| HBAO.Directions | -1 | -1 inherit, 2..16 azimuths |
| HBAO.Steps | -1 | -1 inherit, 2..16 samples per azimuth |
| XeGTAO.Slices | -1 | -1 inherit, 1..8 integration slices |
| XeGTAO.Steps | -1 | -1 inherit, 1..16 per half slice |
| XeGTAO.Falloff | 0.615 | radius fraction, 0.05..1 |
| XeGTAO.ThinOccluder | 0 | compensation, 0..0.7 |
| CACAO.Quality | -1 | -1 inherit, native 0..4; 4 is adaptive |
| CACAO.Power | 1.5 | native exponent, 0.5..5 |
| CACAO.Clamp | 0.98 | native maximum occlusion, 0..1 |
| CACAO.Detail | 0.5 | native detail shadow strength, 0..5 |
| CACAO.AdaptiveLimit | 0.45 | adaptive sample budget, 0..1; native quality 4 |

| Preset | SSAO samples | HBAO directions x steps | XeGTAO slices x steps | CACAO native quality | Denoise passes |
| --- | ---: | --- | --- | --- | ---: |
| Low | 8 | 4 x 4 | 1 x 2 | 1 Low | 1 |
| Mid | 16 | 6 x 6 | 2 x 2 | 2 Medium | 2 |
| High | 32 | 8 x 8 | 3 x 3 | 4 Highest/adaptive | 3 |

Intensity and fade are applied once. CACAO applies its native multiplier, exponent,
clamp and fade before its native blur; other implementations apply intensity/fade
at final resolve. Equal radius/intensity values therefore do not promise identical
darkness across algorithms. XeGTAO keeps Intel's radius multiplier 1.457 and final
power 2.2; those tuned constants are not additional user settings.

HDR integration seam: WBOIT composition now outputs linear `RGBA16_FLOAT`.
XeSS consumes/outputs HDR with inverted depth and NDC velocity; its LDR flag is
removed. `GDX12OutputToScreenPass` tone maps and encodes sRGB once to the existing
UNORM backbuffer. `RenderPipelineCommonData::ResetUpscalerHistory` starts true,
becomes true on AO setting changes/resize, and is consumed after successful XeSS
execution. Camera-cut callers can set the same flag. External CACAO/XeSS calls
invalidate command-list binding caches before engine drawing resumes. These
small shared changes should be merged only once with other rendering features.

Build/test commands and measured results are recorded in `ao-validation.md`.

## Execution point

`Render.AO.ExecutionPoint` (runtime/INI, default 1) selects 0 immediately after the depth+normal prepass, or 1 immediately before deferred lighting. Early mode writes normal-mapped world normals in the prepass. Late mode runs a depth-only VS/PS with masked alpha-test, without normal/material G-buffer evaluation or color targets; normals are produced by the later G-buffer. Early mode does not read last frame's G-buffer. CSM and local shadows execute after the prepass (and early AO when selected). The G-buffer reuses prepass depth with Equal testing and no depth writes.

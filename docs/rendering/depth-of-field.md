# FidelityFX depth of field

> Central REVIEW acceptance and remaining limits: [rendering-review.md](../rendering-review.md). Historical branch/handoff statements below are retained as history.

Status: implementation handoff, **final verification deferred** by the user's
latest instruction. Existing fixtures and development diagnostics are retained;
the owning chat must build, review and validate the final combined scope. The
commands below describe the available checks, not a verification receipt.

The single-GPU pipeline uses AMD FidelityFX DoF 1.1 from SDK 1.1.4, pinned to
`c6efa6bf7f2027b3ec94f28578bb5965eabb9e55`. No Cauldron or old depth-band/PatchMatch
implementation is included. `tools/Build-FidelityFX.ps1` downloads the upstream
MIT-licensed archive, checks SHA-256, extracts only the SDK and license into this
checkout's ignored `Build/dependencies`, and builds only DoF and the DX12 backend.
The renderer's MSBuild target runs this preparation before compilation. CMake and
VS 2026 are required; first preparation needs network access. No SDK binaries are
committed. The script preserves upstream sources unchanged.

## Runtime settings

All variables are registered once in `GDX12DepthOfField.cpp` with `Default` flags,
before INI loading. Set them from `Config/Engine.ini` or through the existing
`ConsoleModule::FindConsoleVariable(...).SetFromString(...)` / typed `SetValue` API.
Non-finite, out-of-range and invalid enum values are rejected without changing the
previous value. The pass reads a value snapshot once per frame.

| CVar | Default | Unit / accepted range |
| --- | --- | --- |
| `Render.DOF.Enabled` | `true` | bool; false copies the HDR input exactly |
| `Render.DOF.Quality` | `2` | integer: 0 Off, 1 Low, 2 Mid, 3 High |
| `Render.DOF.FocusDistanceCm` | `500` | positive camera-to-focus-plane distance, 1..1,000,000 cm |
| `Render.DOF.FNumber` | `2.8` | lens f-number, 0.7..64 |
| `Render.DOF.SensorHeightMm` | `24` | vertical sensor size, 1..100 mm |
| `Render.DOF.CocLimit` | `0` | full-resolution CoC radius / image height, 0..0.25; 0 inherits preset |
| `Render.DOF.Rings` | `0` | maximum SDK kernel rings, 1..12; 0 inherits preset |
| `Render.DOF.RingMerge` | `true` | SDK bilateral-mip ring merging |

Low/Mid/High select 3/5/8 rings and CoC limits 0.025/0.05/0.1. These change actual
kernel work and image results. The ring count is a maximum; the SDK adapts samples
to CoC and merges outer rings when enabled. Off and Enabled are independent of the
saved overrides. Quality changes never write focus/aperture/sensor or manual
Rings/CocLimit. Writing 0 to an override restores preset inheritance. There is one
implementation, so there is no empty implementation selector.

Focus, aperture and sensor edits update dispatch coefficients without recreating
the SDK context. Effective rings, CoC limit or RingMerge changes recreate it after
waiting for previous GPU work. Resize destroys the old context after a fence;
normal disable/re-enable keeps it cached. Targets below 16 pixels on either axis
use the exact-copy path because the SDK allocates four half-resolution mip levels.
The SDK backend buffers four frames. More than four engine frame slots use a queue
flush before DoF dispatch to avoid overrunning that SDK allocation contract.

## Optical and HDR contract

Scene/view units are centimetres. Camera FOV is vertical, in degrees. A valid RH
reverse-Z perspective camera with FOV 1..179 degrees is required. Let sensor height
in centimetres be S, vertical FOV theta, focus distance F, f-number N and view-space
object distance D > 0. The lens focal length is `f = S / (2*tan(theta/2))`, aperture
radius is `f/(2*N)`, and signed sensor CoC radius is
`r = f*f/(2*N*(F-f)) * (F/D - 1)`.
A focus plane at/before the focal length is clamped to `f + 0.01 cm` for a finite,
physical mapping. This affects the frame snapshot, never camera/author fields.

The SDK operates at half resolution: multiply sensor CoC by `0.5*height/S`.
With the engine's row-vector projection, view Z is negative and device depth is
`(viewZ*P33 + P43)/(viewZ*P34)`. The resulting affine device-depth CoC has positive
near blur, zero at focus, negative far blur. Do not substitute axial near/far
normalization or a diameter for the aperture radius. The projection mapping is
independent of jitter; render-grid color/depth share the same camera jitter.

The shared integration seam is deliberately small:

1. WBOIT composition outputs **linear RGBA16F HDR** on the native render grid.
   It no longer tone maps or encodes sRGB.
2. DoF consumes this HDR plus the matching opaque reverse-Z depth, and outputs
   linear RGBA16F on exactly the same grid. Off copies all channels bit for bit.
3. XeSS consumes linear HDR (LDR flag removed), matching depth and unjittered NDC
   motion; output is full-resolution linear RGBA16F with UAV allocation.
4. `GDX12OutputToScreenPass` applies the existing Reinhard + sRGB transfer once,
   writes RGBA8 UNORM, and presents. Output alpha is 1.

DoF/XeSS invalidate cached command-list state after SDK calls, so later passes
rebind heaps, root signatures and PSOs. FidelityFX's unregister callback restores
external resources to the states supplied at dispatch; the engine tracker keeps
those same states. A UAV barrier orders each SDK output before reuse. Ordinary
texture allocation now checks its HRESULT rather than continuing with a null
resource. This includes the pre-existing one-line warning fix in that touched file.

XeSS history resets on first use, resize, camera selection/projection changes,
DoF enable/disable, optics changes and context recreation. Code that teleports the
same camera must set the existing `RenderPipelineCommonData::ResetUpscalerHistory`
flag explicitly. DoF itself is spatial and has no temporal history. Smoothly
animating focus currently resets XeSS on each changed frame; this favors correctness
over temporal accumulation while focus is moving.

For merging other techniques, preserve one HDR composition and one final tone map.
The SDK state-cache invalidation and XeSS HDR/UAV/history fixes are shared changes;
do not duplicate them as separate HDR managers.

## Checks

Run from this clone. Use VS MSBuild at
`C:/Program Files/Microsoft Visual Studio/18/Professional/MSBuild/Current/Bin/MSBuild.exe`.

```powershell
$env:_CL_='/Z7 /GL-'
& $msbuild Apps/App/App.vcxproj /p:Configuration=Release /p:Platform=x64 /p:PreferredToolArchitecture=x64 /m:1 /nr:false
& $msbuild Apps/App/DepthOfFieldTests.vcxproj /p:Configuration=Release /p:Platform=x64 /p:PreferredToolArchitecture=x64 /m:1 /nr:false
& ./Build/bin/x64/Release/DepthOfFieldTests/DepthOfFieldTests.exe --cpu
& ./Build/bin/x64/Release/DepthOfFieldTests/DepthOfFieldTests.exe
& ./Build/bin/x64/Release/DepthOfFieldTests/DepthOfFieldTests.exe --five-frames
& $msbuild Apps/App/BistroDofSmoke.vcxproj /p:Configuration=Release /p:Platform=x64 /p:PreferredToolArchitecture=x64 /m:1 /nr:false
& ./Build/bin/x64/Release/BistroDofSmoke/BistroDofSmoke.exe
```

`_CL_` is the local toolchain workaround from the common rendering contract, not
a global toolchain change. Set `$msbuild` to the path above. Tests are console
applications imported from the App project, with no production-only test hooks.

The CPU check loads INI, checks runtime flags and failed writes, checks preset
inheritance, and compares CoC against an independent image-plane thin-lens
calculation at four FOVs and four resolutions. GPU tests use real readback to
check exact Off/in-focus, near/far blur, focused interior, HDR bokeh, foreground
silhouette/color leakage, runtime FOV/ring merging, deterministic re-enable,
3/5 buffered frame slots, odd resize/restore and tiny-target bypass. The buffered
check queues 32 frames with per-slot fences rather than flushing every frame.
XeSS tests exercise matching 493x277 -> 640x360 grids, HDR output, focus reset,
641x361 resize and context recreation. RGBA8 readback checks final tone mapping
against known expected pixel values. D3D12 warnings/errors fail the fixture.

Both GPU executables hold `Local\PEPEngine2FeatureGpuValidation` for their entire
run. Timing samples measure the DoF pass only, with 16 measured frames and GPU
queue completion per sample; they exclude readback, XeSS, scene rendering, context
creation, and presentation. The ordering comparison warms up separately and uses
16 measurements after 8 warmups. They are not FPS measurements. DXGI local memory
is whole-process usage after readback, including the engine's large descriptor
heap and driver caches; it is not incremental DoF allocation or total VRAM usage.

`BistroDofSmoke` loads the real Exterior and Interior. It changes only the existing
camera pose, selects an existing liquor bottle, freezes movement for comparable
images, and changes CVar values from C++ after startup. It creates no scene
entities/assets. Each quality captures the accessible XeSS HDR output after GPU
completion into `Build/bistro-dof-q*.ppm`; this is actual Bistro rendering, separate
from the synthetic fixture. Production Bistro movement/draws are unchanged.

## Known limits

Opaque single-layer depth cannot represent glass surfaces or several transparent
layers. A transparent bottle may be blurred according to the opaque surface behind
it; WBOIT color cannot supply missing layer depths. This is not glass-aware DoF.
DoF before XeSS preserves original color/depth correspondence and avoids depth
upsampling, but very small highlights are less regular than the full-resolution
comparison. Low ring counts show sampling structure. The ordering fixture uses
analytic full-resolution depth for comparison only; production does not invent a
resampled full-resolution depth field. No autofocus, anamorphic aperture, camera
exposure simulation, new scene geometry, multi-GPU changes or extra post framework
is included.

Sources: [AMD DoF manual](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/depth-of-field/),
[pinned SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/tree/c6efa6bf7f2027b3ec94f28578bb5965eabb9e55).

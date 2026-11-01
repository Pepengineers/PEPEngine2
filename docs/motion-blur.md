# Feature-aware motion blur

> Central REVIEW acceptance and remaining limits: [rendering-review.md](rendering-review.md). Historical branch/handoff statements below are retained as history.

Status: implementation handoff. The user stopped verification on 2026-11-01.
The final integrated state is **unverified**; the main chat owns subsequent
builds, tests, GPU measurements and review. Existing verification code is retained.

The single-GPU App pipeline is now:

`opaque G-buffer + lighting + WBOIT -> linear HDR composition -> motion blur -> HDR XeSS -> tone mapping -> UNORM back buffer`.

The filter is an independent implementation of Guertin, McGuire and
Nowrouzezahrai, **A Fast and Stable Feature-Aware Motion Blur Filter**, final
HPG 2014 paper, Sections 4–5 and Appendix A:
[author page](https://casual-effects.com/research/Guertin2014MotionBlur/index.html),
[final paper](https://casual-effects.com/research/Guertin2014MotionBlur/Guertin2014MotionBlur-small.pdf),
[DOI](https://doi.org/10.2312/hpg.20141093).
No donor implementation or superseded commits were copied. The paper and its
figures remain copyright © 2014 their authors/publishers; they are not distributed
with this implementation. The equations were implemented against the final paper,
including its Appendix A, rather than the 2013 technical report.

## Algorithm and conventions

`GDX12MotionBlurPass` accepts matching-grid R16G16B16A16_FLOAT linear HDR,
reverse-Z depth, and R16G16_FLOAT motion. Output 0 is matching-grid linear HDR;
output 1 is complete unjittered NDC motion for XeSS. This second output includes
sky motion even when artistic blur is disabled.

Opaque motion is **previous NDC minus current NDC**, with X right and Y up,
computed from previous/current object worlds and unjittered view projections.
It maps current pixels into the previous frame, as required by XeSS. Displacement
in output pixels is `motion * (outputWidth/2, -outputHeight/2)`.
The filter uses half that displacement times ShutterFraction, so exposure is
centered about the current frame. ShutterFraction is exposure time divided by
frame duration; 0.5 means 180 degrees. There is no additional delta-time multiplier.

The half-exposure vector is clamped by magnitude in **output pixels**, then
mapped into the working input grid. The tile side is the ceiling of the largest
working-grid radius (at least 8 pixels to avoid tiny-tile dispatch overhead), so
only the eight neighboring tiles can contribute. A
256-thread reduction handles partial tiles without out-of-bounds reads. NeighborMax
rejects off-axis diagonal motion; the gather stochastically selects only axial
neighbor lookups at tile boundaries.

The gather splits taps between the dominant direction and the local/perpendicular
direction, uses direction-aware weights for foreground spreading, background
reveal and the overlapping-cylinder correction, and normalizes the center weight
by the tap count. It retains the midpoint sample. Like the paper's main results,
it uses the conservative 50/50 distribution rather than an extra variance pass.
Halton jitter is spatially deterministic; its paper defaults are h=0.95, phi=27,
with jitter bounded to remain inside the shutter interval at low sample overrides.
Both line directions are canonicalized and angular weights use absolute dot
products because a centered exposure must be invariant to velocity sign.

RH reverse-Z is converted into positive view distance in scene centimeters:
`near*far / (near + depth*(far-near))`. The relative depth comparison from Section 5
is expressed in these positive distances (equivalent to applying it to reciprocal
distance), so a distant moving sample cannot leak through a stationary foreground.
Weighted color *differences from the center* avoid FP16 drift on constant HDR.

Sky motion reconstructs a homogeneous world direction from the jittered inverse
view projection and reprojects it with **w=0** through current/previous unjittered
view projections. It responds to rotation, excludes translation, and does not
turn projection jitter into motion. Geometry whose previous clip W is behind the
camera receives zero motion instead of an unbounded displacement.

The RH projection uses `_31 -= jitterX*2/inputWidth`,
`_32 += jitterY*2/inputHeight`. With `clipW=-viewZ`, these produce positive raster
pixel offsets `(jitterX,jitterY)`. Those same offsets go to XeSS, while motion
excludes jitter. See [Intel's integration guide](https://github.com/intel/xess/blob/main/doc/xess_sr_developer_guide_english.md).

## Runtime CVars

All are owning static registrations in one `.cpp`, registered before INI load,
with `Default` flags. Writes from C++ through ConsoleModule work;
finite/range checks reject bad values without replacing the last valid setting.
The pass reads one validated settings snapshot per frame.

| CVar (`Render.MotionBlur.` prefix) | Default | Valid values / units |
| --- | --- | --- |
| Enabled | true | bool; false bypasses artistic blur |
| Quality | 2 | 0 Off, 1 Low, 2 Mid, 3 High |
| ShutterFraction | 0.5 | 0..1 exposure/frame; 0 is neutral |
| MaxRadius | 40 | 1..128 output pixels, half-exposure radius |
| Samples | 0 | 0 inherits quality; 4..128 explicit gather taps |
| CenterWeight | 40 | 1..100, paper k; center weight = N/(k*radius) |
| VelocityThreshold | 1.5 | 0.1..8 input pixels beyond the 0.5 pixel floor, paper gamma |

Low/Mid/High execute 19/27/35 gather taps. Quality does not write any parameter
CVar. A nonzero Samples override survives quality changes; setting it back to 0
restores the preset. Other controls are independent of quality. A single algorithm
has no artificial implementation selector.

```cpp
ConsoleModule console;
console.FindConsoleVariable(L"Render.MotionBlur.Quality").SetFromString("3");
console.FindConsoleVariable(L"Render.MotionBlur.Samples").SetFromString("47");
```

Set the same names under `[ConsoleVariables]` in `Config/Engine.ini` beside the
executable. MSBuild only copies the repository template when that destination
does not exist, as described in [console-variables.md](console-variables.md).

## Reset and resource lifetime

The blur has no color history. Off and zero shutter copy HDR exactly; an enabled
stationary gather also returns the source pixel exactly. The first frame after
enable, resize or a temporal reset is neutral. Enabled/Off transitions also reset
XeSS. Radius changes safely flush outstanding use before replacing tile textures;
ordinary quality/sample changes use root constants and do not rebuild PSOs.

On camera teleport/cut, set `CameraComponent::ResetHistory = true` before its GPU
update. Active-camera switches, resize, projection changes, first use and frame
gaps over 250 ms also request a reset. The camera update consumes it once, updates
previous projection to current, and publishes ResetHistory to blur and XeSS.
XeSS independently resets its first execution after context initialization/resize.

Transform updates include one final buffered-frame cycle after motion stops so
every slot receives `previousWorld == currentWorld`; paused simulations still
update GPU histories. The pass uses root constants, the existing per-frame camera
CB, and five textures on one ordered direct queue, without a separate frame ring.
UAV outputs transition to SRVs and are rebound after writes to satisfy root
signature 1.1 lifetime requirements. XeSS native recording invalidates the existing
command-list state cache before the next engine pass.

## HDR integration seam

Composition now writes R16G16B16A16_FLOAT **without tone mapping**. XeSS receives
that HDR format, INVERTED_DEPTH and USE_NDC_VELOCITY, with no LDR flag, and has a
real UAV output. `GDX12OutputToScreenPass` performs the existing Reinhard + sRGB
encoding exactly once into the UNORM back buffer. It allocates no intermediate
tone-map texture. Other effect branches can share these two small HDR changes
when integrated; merge them once, then insert effects between composition and
the final tone mapping.

Blur runs before XeSS so color/depth/object motion use the same raster grid,
including jitter, and XeSS can suppress its high-frequency sampling noise. The
fixture compares that actual HDR XeSS path with native-resolution filtering and
measures both working grids; it does not substitute a resized CPU image for XeSS.
The technique's output radius stays in output pixels in either case. Validation
of the final integration and its visual limitations belongs to the main chat.

## Verification

From a PowerShell prompt in this clone:

```powershell
$env:_CL_ = '/Z7 /GL-'
$msbuild = 'C:/Program Files/Microsoft Visual Studio/18/Professional/MSBuild/Current/Bin/MSBuild.exe'
& $msbuild Apps/App/App.vcxproj /p:Configuration=Release /p:Platform=x64 /p:PreferredToolArchitecture=x64 /m:1 /nr:false
& $msbuild Apps/App/MotionBlurTests.vcxproj /p:Configuration=Release /p:Platform=x64 /p:PreferredToolArchitecture=x64 /m:1 /nr:false
& $msbuild Apps/App/ObjectDataJobsTests.vcxproj /p:Configuration=Release /p:Platform=x64 /p:PreferredToolArchitecture=x64 /m:1 /nr:false
New-Item -ItemType Directory -Force Build/MotionBlur | Out-Null
Build/bin/x64/Release/MotionBlurTests/MotionBlurTests.exe --cpu
Build/bin/x64/Release/MotionBlurTests/MotionBlurTests.exe
```

The GPU test acquires `Local\PEPEngine2FeatureGpuValidation` for its entire GPU
lifetime. Exit 75 means another feature owns the mutex; retry after it finishes.
Other App/GPU tests must be run under the same external mutex. Release checks
remain active: they throw on failures rather than relying on disabled `assert`.
Images/reports are written only to this clone's ignored `Build/MotionBlur`.

The fixture checks exact stationary/Off/zero-shutter identity, both velocity signs,
foreground spreading, static foreground occlusion, perpendicular motion, sky
rotation versus translation/jitter, odd dimensions, resize and radius changes,
all camera frame slots and queued frames, real quality timings, native/XeSS HDR images, temporal
jitter convergence and the same-list blur -> XeSS -> tone-map chain. It rejects
all debug-layer warnings/errors after the engine's pre-existing empty-cache setup.
The object-data fixture independently checks the actual application jobs and GPU
constant buffers for stopped-object motion and camera-cut/projection resets.

## Limits

This is a single-layer screen-space, constant-velocity reconstruction. It cannot
recover hidden color or curved trajectories; accelerating objects and large
rotations retain the paper's limitations. WBOIT is preserved, but transparent
layers inherit the opaque/background motion/depth already available in the App's
G-buffer. FP16 velocities have subpixel rounding. No entities or demo scenes were
added to Bistro. Synthetic image/timing tests are not a visual evaluation of the
entire Bistro scene, and pass timings do not imply an application FPS target.

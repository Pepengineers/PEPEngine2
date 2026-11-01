# Local reflection probes

`ReflectionProbeComponent` plus `TransformComponent` enables a probe; neither the component nor the renderer creates scene content. Bistro is unchanged. The renderer supports 32 enabled probes on the primary GPU. Exceeding this limit or supplying invalid author parameters throws before capture.

## Authoring

All positions and distances are world centimetres. `TransformComponent.Location` is the centre of an axis-aligned influence/projection box. `Extents` (default 500 cm per axis) is its half size; transform rotation/scale do not rotate/scale the box. `CaptureOffset` (default zero, world axes) offsets the capture centre, which must remain inside the box. Box bounds and capture location must be finite, with strictly positive box size.

`BlendDistance` defaults to 100 cm and must be finite and positive. Coverage fades from zero at a box boundary to one at that distance inside it. Overlap weights are normalized; uncovered weight uses global specular IBL. `Intensity` defaults to 1, range 0..16. Diffuse irradiance stays global; local prefiltered incident specular radiance replaces its share of global specular radiance before the existing split-sum BRDF, never adds a second specular term.

`NearPlane` defaults to 1 cm, minimum 0.01 cm. `FarPlane` defaults to 10000 cm and must be greater than near and at most 1000000 cm. `UpdateInterval` defaults to 1 frame, range 1..3600, and is the minimum interval between complete generations. `Enabled` defaults to true. `Invalidate()` increments the probe revision for an explicit recapture. Author fields are not modified by quality settings.

Mesh classification is explicit through `StaticMeshRenderComponent.ReflectionDynamic` (default false), or inferred from existing velocity, circle, spline-follow, flying-prop and look-at components. The resolved classification is uploaded per instance. An otherwise static object whose transform changes invalidates the static cache; an object intended to move every frame should be classified dynamic.

## Global CVars and component capture fields

Only `Render.Reflection.Enabled` (boolean, default true) and `Render.Reflection.Quality` (0 Off, 1 Low, 2 Mid default, 3 High) are global writable `Default` CVars. They are registered once in `GDX12ReflectionProbes.cpp` before startup INI loading. Use ConsoleModule from C++ or `Config/Engine.ini` to change them.

Every `ReflectionProbeComponent` owns the capture settings below through `GDX12ReflectionCaptureSettings`. Change the fields on that component from C++; these are not global CVar overrides. The renderer copies each component into its frame submission and resolves that probe independently.

| Component field | Default | Accepted values / units |
| --- | --- | --- |
| `Implementation` | 1 | 0 full dynamic, 1 baked static color + matching depth + dynamic overlay |
| `Resolution` | 0 | 0 inherit quality; power-of-two face width 16..512 pixels |
| `FacesPerFrame` | 0 | 0 inherit quality; 1..6 capture faces when this probe is scheduled |
| `PrefilterSamples` | 0 | 0 inherit quality; 8..1024 GGX samples per texel |
| `UpdateInterval` | 1 | 1..3600 frames minimum between generations of this probe |

| Quality | Inherited resolution | Inherited faces/frame | Inherited GGX samples |
| --- | --- | --- | --- |
| Low | 64 | 1 | 32 |
| Mid | 128 | 2 | 64 |
| High | 256 | 6 | 128 |

Explicit component values persist across quality changes. Set Resolution/FacesPerFrame/PrefilterSamples back to zero to restore preset control. Implementation and UpdateInterval always belong to the component. The existing Enabled, bounds, capture offset, intensity, blend distance, near/far and explicit revision also remain individual author fields. Invalid component values are rejected before capture.

The retired global names `Render.Reflection.Implementation`, `.Resolution`, `.FacesPerFrame`, `.PrefilterSamples` and `.UpdateInterval` must be removed from existing INI files; move their intended values to the relevant component. The source template and the existing generated App/test configs in this clone were migrated. MSBuild deliberately does not overwrite an existing runtime INI.

Off writes neutral metadata in each reused frame slot and performs no capture. A frame snapshots the two global CVars and every submitted probe's author settings. Resource replacement waits for preceding GPU work before replacing descriptors.

## Capture, caching and publication

Captures use RH cameras, D3D cubemap face orientation, reverse-Z depth (clear zero), RGBA16F linear HDR, the existing PBR material sampling, directional/local lighting and global IBL. Transparent geometry is excluded from capture; masked alpha uses the same 0.5 threshold as the opaque pass. Draws are GPU-frustum-culled and use ExecuteIndirect.

Dynamic mode captures every opaque/masked instance. Baked mode captures static color and its matching D32 depth for each face, restores both, then draws only dynamic instances against the restored depth. Static geometry/transform/classification changes, material `DirtyFlag` updates, lighting changes, sky revision and explicit probe invalidation rebuild the bake. Only revisions of materials referenced by static meshes invalidate the bake; dynamic-only material edits do not restart a generation. Material edits must use the existing `DirtyFlag` contract so their GPU material data is also updated. A changed texture's contents require `GDX12EnvironmentIBLPass::Invalidate()` after the sky texture upload (rebuilds IBL and increments its revision), or explicit probe invalidation for other texture changes; normal asset pointer/material edits follow material dirty handling. World/probe replacement drops publication eligibility for the old scene. Deleted entities have zero indirect commands, including on world unload. Runtime transform/instance buffer growth waits for preceding GPU work; upload buffer growth preserves existing contents so adding a probe entity cannot erase the scene transforms.

One working raw cubemap and one GGX output cubemap are allocated at the largest enabled resolution. Each scheduled probe renders, filters and restores depth through the mip subresources matching its own resolution. Fixed per-mip source views avoid reallocating scratch textures or rewriting shader-visible descriptors when switching between differently sized probes. Each baked probe owns color and matching depth at its own resolution, so depth restoration copies complete, equally sized subresources. The published cube array uses the largest enabled probe resolution; smaller captures occupy a lower mip range, and per-probe MinMip/MaxMip bounds keep sampling inside their published data. A change to that shared array's maximum size republishes the probes while preserving unchanged per-probe static caches. Topology changes rebuild the shared array and caches. Only the final copy of all six faces and every roughness mip changes the published cube array. Published box/capture coordinates stay with that generation. Previous generations remain readable while another is built; dynamic motion does not reset the face cursor. Static invalidation abandons the incomplete generation to avoid combining stale static color/depth. Updates visit probes round-robin; a full cycle needs approximately the sum of `ceil(6 / probe.FacesPerFrame)` over the enabled probes frames, excluding author interval waits. These are persistent scene probes, so main-camera cuts do not invalidate their capture.

Sun shadows use a separate 1024-square orthographic map fitted to the whole scene bounds, including off-camera casters. Main-camera CSM is never reused for probe faces. Static bake shadows contain static casters only, including separately filtered local point/spot maps. Dynamic overlay lighting uses all casters. This policy keeps the bake stable while objects move: **moving objects do not cast new shadows onto already baked static receivers**. Use full dynamic capture for that case. Local shadow caches now invalidate when their caster set changes.

## HDR and SSR integration seam

The shared HDR change is independent of probe capture: deferred lighting and transparent accumulation feed an RGBA16F WBOIT composition; XeSS receives linear HDR with `XESS_INIT_FLAG_LDR_INPUT_COLOR` removed; the final screen pass alone applies `ToneMapToSRGB`. XeSS starts with reset history, resets on resize, active camera change/`CameraComponent.ResetHistory`, probe enable/disable and probe topology/resolution/mode switches. Native XeSS recording invalidates the command-list state cache before the screen draw.

`SampleReflectionRadiance(positionWorldCm, reflectedDirectionWorld, roughness, globalPrefilteredRadiance)` in `ReflectionProbes.hlsli` is the SSSR miss fallback. It returns **incident linear HDR radiance**, including normalized local/global coverage and box projection. The SSSR resolve should replace the current prefiltered-radiance lookup in `ShadeSunAndIBL`, choose SSR hit radiance or this fallback, then apply the existing BRDF factor exactly once. Do not add another specular resolve over already shaded scene color. The cube-array SRV and per-frame `GDX12ReflectionProbeData` buffer carry the published captures and matching boxes; unready/disabled entries have zero intensity. Deferred bindings are t12/t13 (AO is t14); transparent bindings are t9/t10 (material textures begin at t11).

Transparent surfaces consume probes in forward PBR and retain WBOIT weighting. Capture excludes transparent surfaces, so glass cannot reflect glass layers from these maps. WBOIT remains an approximation for overlapping glass and does not model refraction or multiple specular bounces. Capture itself uses global IBL, avoiding recursive probe feedback. The single-level source cubemap is GGX importance sampled directly; low sample counts can alias small bright features.

## Verification

Build from this clone only, with Release/x64, PreferredToolArchitecture=x64, `/m:1 /nr:false`; the host MSVC workaround is `_CL_=/Z7 /GL-`.

```
MSBuild Apps/App/App.vcxproj /p:Configuration=Release /p:Platform=x64 /p:PreferredToolArchitecture=x64 /m:1 /nr:false
MSBuild Apps/App/ReflectionProbeTests.vcxproj /p:Configuration=Release /p:Platform=x64 /p:PreferredToolArchitecture=x64 /m:1 /nr:false
Build/bin/x64/Release/ReflectionProbeTests/ReflectionProbeTests.exe --cpu
Build/bin/x64/Release/ReflectionProbeTests/ReflectionProbeTests.exe
Build/bin/x64/Release/ReflectionProbeTests/ReflectionProbeTests.exe --benchmark
```

The fixture acquires `Local\PEPEngine2FeatureGpuValidation` for its entire GPU lifetime. Default runs enable the DX12 debug layer and GPU-based validation; `--benchmark` disables instrumentation. GPU timestamps cover reflection capture + sun shadows + GGX/publication; they exclude environment initialization, main local-shadow work, CPU submission/readback and presentation. The reported allocation bytes cover the published array, shared capture/GGX/depth/sun resources and each allocated baked color/depth pair; they exclude descriptor heaps, frame buffers and separate local-shadow maps. The isolated fixture is not a visual review of Bistro.

The component ownership and mixed-resolution resource changes have not been built, run or reviewed in a separate verification pass, following the latest instruction to leave verification to the main chat. The existing fixture callers were adapted to component settings; no new checks were created for this change.

## Integration review

The centralized REVIEW copy connects this fallback to the actual SSSR SDK and its final subtraction. Capture settings remain per component; the GPU oracle also publishes two probes with different resolutions, modes and face/sample budgets. Current verification and limits are recorded in [rendering-review.md](rendering-review.md).

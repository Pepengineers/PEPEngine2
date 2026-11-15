# Bistro mesh transparency in MetaRenderGraph

## Scope and integration

The ordinary Bistro path now follows opaque depth/GBuffer, sun/local shadows,
AO and deferred lighting with one mesh WBOIT `DepthPartition::All` draw,
one WBOIT resolve and the existing HDR display transform/PRESENT.
`SceneTransparency` owns accumulation, revealage and composed HDR per frame slot.
It reuses `GDX12WBOITTransparencyPass`, `GDX12WBOITCompositionPass`,
`TransparentPass.hlsl` and `CompositionPass.hlsl`; it records native commands
through Meta accesses/barriers and Executor, without legacy pass Execute calls.

GPU culling already produces transparent indirect commands/counter in
SceneGeometry buffers 6/7. The new owner consumes that stream without readback,
CPU object lists or sorting. `TRANSPARENT_UNBATCHED` preserves the source instance
ID instead of interpreting it as an opaque batch remap. Accumulation and revealage
always clear to 0 and 1; zero visible draws therefore resolve to the original HDR.
Read-only opaque depth and reverse-Z depth test are retained, with no depth writes.
Existing alpha (texture alpha times material opacity), weight and blending remain
unchanged. Alpha-tested cutouts remain opaque.

Forward PBR uses all published local lights, sun/local shadow maps and static IBL.
The light SRV is the same frame's `ShadowLights` used by deferred lighting, including
cached stable `ShadowFirstSlice` and per-face masks. There is no opaque-depth light
grid or screen AO applied to glass. The new opt-in static-IBL shader variant omits
probe and sky/aerial reads; default App variants preserve their prior behavior.
Composition uses the existing `SKIP_OPAQUE_AERIAL` variant, so no atmosphere or
repeated aerial composition is added.

The owner refreshes input links every plan, uses the slot's read-only DSV, declares
geometry/indirect/lighting/depth/RT accesses and resizes its existing targets when
the render extent changes. Recording closures hold immutable native bindings.
No per-frame Flush, readback or additional scene upload was introduced.

## Shadow receivers

`SceneLocalShadows` now includes blended bounds in both initial BVH construction
and incremental membership. Receiver selection can therefore request faces needed
by transparent surfaces. Caster lists still require `CastsShadows` and exclude
RenderLayer=2; blended meshes never become opaque shadow casters.

Opaque/blended layer changes update the existing leaf and caster flag, invalidate
old/new caster bounds and refit ancestors. Material/transform/instance reverse
references and scene/selection revisions remain in use; visibility/topology
changes retain the existing rebuild path. Moving the camera does not cause a
whole-scene input scan. Solar CPU bounds and `ShadowPass.hlsl` caster culling
already exclude layer 2 and were left unchanged.

## Where to look

Real entrance door/window glass and menu covers are visible from the initial
camera. Wine bottles on outdoor tables and glass/bottles behind the interior bar
are further examples. Source MTLs include exterior `MASTER_Glass_Exterior` (d=0.2),
`MenuSign_02_Glass` (d=0.39), `Paris_LiquorBottle_01_Glass_Wine` (d=0.65), and
interior `MASTER_Interior_01_Glass` (d=0.2). Materials, textures and opacity were
not edited; no synthetic glass was added. This is ordinary blended transparency,
without refraction, SSR or local reflection probes.

## Verification and limits

Release x64 build passed in the Visual Studio Developer Prompt outside the
sandbox (the PDB-access limitation was established during the preceding AO task).
Build retained external-library missing-PDB LNK4099 warnings.
One ordinary no-argument launch on 2026-11-15 displayed the entrance glass and,
after camera movement, a transparent bottle on an outdoor table. Background was
visible through glass while opaque door frames, table and bottle label occluded
it; no obvious visual failure was observed. GPU-rendered frames continued to
present. The window was left open for the user's inspection; the subsequent log
records normal resource shutdown and release.

Both operation dumps add only `mesh transparency All clear/draw` and
`mesh WBOIT resolve` between deferred lighting and HDR display. Existing AO/shadow
packets remain, and the excluded techniques do not execute. Current-run logs
contain no ERROR/FATAL. `git diff --check` and independent source review passed.
Build/run logs, operation dumps and review artifacts are under
`Build/Verification/meta-bistro-transparency/`.

Existing WBOIT composition coverage in `Apps/App/tests/AOTests.cpp` was
located; no tests were added, changed or run. No `--smoke`, captures, profiling,
quality matrices, precision tests or repeated runs were performed. Zero-count,
resize and incremental layer/visibility transitions were reviewed in source,
not exercised with artificial scenes. Full image acceptance and FPS/CPU/GPU cost
remain for the user; no measured technique cost or speedup is claimed.

Camera defaults, scene, lights, AO settings, shadow parameters, native resolution,
primary adapter and paused simulation remain unchanged. Water, particles, grass,
clouds, animation, physics, SSR, probes, refraction, post effects, reconstruction
and frame generation remain outside this stage. The pre-existing working changes
in `SceneManagerModule.cpp`, `BistroAdditionsTests.cpp` and `Temp/` are preserved
and excluded from the commit; the executable includes those existing scene edits.

Executable: `D:/Project/PEPEngine2/Build/bin/x64/Release/App.MetaRenderGraph/App.MetaRenderGraph.exe`.
No next technique is started before user acceptance.

## Follow-up material review

The user requested more visible drinking glasses, then reported the broader
highlights looked better. Only `Paris_LiquorBottle_01_Glass1` was adjusted:
opacity 0.25 -> 0.50 and shininess 5000 -> 100 (imported roughness approximately
0.045 -> 0.14). The OBJ assigns this material to drinking glasses, including
water, wine and beer glasses; window and bottle materials are separate.

The user also reported black bands on the cotton placemats. Their separate
`Paris_Table_cloth_01` material referenced a visually uniform `_ddna` map.
A 64-pixel grid sample across the image returned one RGBA value, (53,53,199,255).
The current RGB normal decoder treats this as a strongly tilted tangent normal,
not a neutral normal. The material's `map_bump` reference was removed so it uses
its geometric normal; diffuse and specular textures remain. This is a narrow
material correction, without changes to AO, shadows or the shared normal decoder.
The application was relaunched for visual review; disappearance of the reported
bands still requires inspection at the affected table. No rebuild is needed for
these MTL edits; the existing importer includes MTL timestamps in its cache key.

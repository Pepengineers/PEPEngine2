# Bistro grass and wind in MetaRenderGraph

## Scene and time

The ordinary Meta application now creates only `BistroGrass::Scene` additions
and updates their existing controls before App's ordinary update. Interior,
exterior, camera defaults, meshes/materials, AO, SSSR without probes, WBOIT,
shadow settings, native resolution and primary GPU 0 remain as before.
Physics, animation, particles and all other deferred additions remain disabled.

`BistroGrassGeometry::CollectScene` has an explicit opt-in to advance while the
world is paused. Only ordinary Meta grass passes this flag. Default App callers
retain the previous pause behavior. World time scale and explicit field pause
remain effective; the existing renderer applies wind TimeScale, MaxStepSeconds,
MaxSubsteps and reset rules. No global unpause or simulation tick was introduced.
There are no synthetic interaction sources; the existing GPU interaction clear
initializes presence/trail/previous-trail to neutral and updates them normally.

## Parameters and viewing

Source Startup.ini selects Render.Grass/Wind.Enabled=true, both Quality=2 (Mid),
Render.Grass.AsyncCompute=false, App.Grass.Count=1, Implementation=0 (Ribbon),
Wind.Algorithm=0 (Analytic). App defaults otherwise remain:

- Original building_02 tunnel/arch patch: centre (1399.6,23.3,1369) cm, yaw 69°,
  half extents (200,530) cm. No demonstration patch or relocated camera.
- Density 1500 blades/m², budget 60000, seed 7919, segments 4.
- Height 40 cm with 0.25 variation, width 3 cm, max bend 20 cm,
  bend response 0.15 s; LOD 1500 cm, far cards 3000 cm, cull 6000 cm.
- CastShadows=true, wind enabled, speed 100 cm/s at 67.5°,
  gust amplitude 0.6, frequency 0.4 Hz, wavelength 500 cm.
- Wind time scale 1, max step 1/60 s and four substeps;
  inherited Mid grid resolution 64 and pressure iterations 20.
  Projected-grid force/damping defaults remain 2/s and 0.2/s.
- Existing BistroGrass material and diffuse source, roughness 0.85.

Look for the original grassy strip inside the adjacent building's arch/tunnel.
Ribbon/CrossedRibbons and Analytic/ProjectedGrid still use the existing CVar paths;
only the selected initial mode is part of runtime acceptance.

## Native packets and frame history

Ordinary `SceneGeometry` owns a `SceneGrass` with diagnostics disabled. It prepares
grass/wind without calling PrepareScene/CollectReflectionScene. Its initial graph
append keeps root uploads, interaction, wind, blade expansion and actual frame
depth/GBuffer draws; it omits all six diagnostic probe targets, their clear/draw
packets and field/probe readbacks. The unused diagnostic renderer is not created.
Grass wind, expansion, arguments and draws use native Meta packets on DIRECT;
no CPU blade generation, visibility readback or legacy Execute is introduced.

Roots stay in DEFAULT buffers, with upload only for a changed slot signature.
Depth and GBuffer use the same frozen frame's field/world/deformation data.
Existing BeginFrame history stores the previous presented frame's world, eye,
wind time/fields and interaction trail. Shadow PrepareView calls do not advance
them. GrassRender writes all five GBuffer attachments, including camera/deformation
motion, before AO/deferred/SSSR consumption. Existing alpha-test, normal/material,
reverse-Z and read-only GBuffer depth behavior are reused.

## Shadows and cache

A small snapshot of the renderer's published grass primitives supplies expanded
bounds, CastsShadows and geometry/material/transform/deformation revisions. It
iterates only grass patches, not the entire primitive registry or mesh scene.

Sun shadows retain cached mesh caster bounds and union grass bounds separately;
grass revisions refresh the atlas without forcing a mesh-bounds scan. Active
cascades append grass draws after mesh draws into their own atlas viewport/DSV,
without another clear. GeometryTarget supports the existing cascade X/Y region.

Local receivers union camera-clipped grass bounds with the mesh BVH query,
including places without a mesh receiver behind the grass. Grass deformation,
movement/removal/disable and revision changes invalidate affected old/new caster
faces' maps; static mesh lists remain valid. Receiver selection uses a separate
revision for bounds/visibility membership, so fixed-bounds wind does not require
another receiver query. Mesh journals, BVH and mesh scene revision are preserved.
Refreshing a face clears its old depth, draws the retained static list, then
current grass. Submitted publishes readiness after all draws. Each shadow view
expands roots against its own frustum; existing Shadow mode bypasses the camera
distance cutoff, so off-camera grass may cast into visible areas.

## Verification and limits

Release x64 build passed in the Visual Studio Developer Prompt outside the sandbox.
The existing external-library LNK4099 missing-PDB warnings remain.
One ordinary no-argument smoke was completed. The user inspected the running
application and confirmed "я посмотрел, все работает" for this grass/wind stage;
that result is accepted without a repeated run. The current-run log contains
no ERROR/FATAL, and records normal resource shutdown/release. GPU work is present
in the operation dumps: interaction update, two Analytic wind stage-6 dispatches,
per-view argument clear/expansion/ExecuteIndirect and the existing AO/SSSR/WBOIT.
The initial entrance view culls the main grass patch, but the dump includes grass
shadow views in the sun atlas and local faces. Grass root uploads appear only in
the initial slot population, not in the first reused-slot dump. No diagnostic
grass probe/readback operations are present.
No --smoke, additional tests, matrices, captures, profiling or soak harness was used.
Existing BistroRenderGraphTests/grass renderer source was inspected for context;
new test coverage is outside the requested verification contract.
The simplification pass found no meaningful changes, and independent source
review found no actionable issues. Review results are stored with the verification
artifacts. `git diff --check` passed.

Source review must be distinguished from visual acceptance: toggles, removals,
old/new shadow bounds, resize, alternate geometry/wind modes and motion precision
are checked in source rather than artificial/runtime matrices. No performance
gain or technique cost is claimed. The user accepts image/FPS/CPU/GPU before
another technique starts; particles are not implemented by this change.

Artifacts: `Build/Verification/meta-bistro-grass-wind/`.
Executable: `D:/Project/PEPEngine2/Build/bin/x64/Release/App.MetaRenderGraph/App.MetaRenderGraph.exe`.
Pre-existing SceneManagerModule.cpp/BistroAdditionsTests.cpp edits and Temp/ are
preserved and excluded from the task commit.

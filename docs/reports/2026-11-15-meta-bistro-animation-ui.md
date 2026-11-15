# Bistro animation/skinning and screen UI

## Actors and time

Ordinary Meta reuses BistroCharacters::Scene and BistroRoutes: 10 people
(MGPUSoldier Soldier.gltf, Take1, scale 2.4) and 5 foxes (KhronosFox Fox.gltf,
Walk, scale 0.55). Startup explicitly selects NPC.Count=10, Fox.Count=5,
PresetCount=-1 for both, Skinning.Enabled=true, Quality=2 (Mid/30 Hz),
PoseRateHz=0 and the existing Instancing=true. These match the current App source
defaults and saved Debug App skinning configuration; no saved actor-count override
was found. Quality presets provide smaller populations separately, but none is
automatically applied. Counts and neighbouring quality are not reduced.

BistroCharacters::Update has an opt-in advanceWhilePaused parameter; existing App
calls retain world-pause behaviour. Only ordinary Meta opts in, using world time
scale and existing parallel route jobs, completed before snapshot collection.
It passes no PhysicsModule, so routes use authored ground without Jolt queries
or simulation. Original interaction sources from people feed existing grass
collection and then the particle interaction dependency, without synthetic sources.

The pipeline separately opts into skeletal pose advancement while world-paused.
RenderModule still evaluates once per render frame, retaining component freeze,
clip speed, world time scale, pose-rate controls, revisions/reset and current/
previous pose settling. World-pause no longer clears velocity for this opt-in path;
camera-history reset still resets pose history. Default App semantics are unchanged.

## GPU geometry and shadows

Ordinary Freeze consumes the existing FreezeSkeletalMeshes work. Frame-slot
palette uploads and skin deformation reuse SceneGeometry/GDX12Skinning recording;
DEFAULT source geometry/skin inputs remain persistent. The existing indirect
instancing, dirty range uploads and conservative pose bounds are retained.
The diagnostic whole-instance CPU snapshot is omitted from ordinary frames.
Geometry accesses and queue ownership order skin uploads/deformation before the
same shared geometry is consumed by depth, GBuffer/velocity and sun/local shadows.
No legacy Execute, private submit/Flush or readback is added.

Pose changes explicitly mark the instance journal even if conservative bounds
stay equal. Local shadow cached input includes the published deformation revision,
so only affected faces invalidate. Sun receives old/new changed caster bounds,
marks intersecting cascades dirty across slots, and clears/draws only those atlas
rectangles. Camera/light/grass/projection changes keep existing full refresh.
The fitted caster bounds conservatively grow between topology rebuilds, avoiding
a full static scene walk each animation frame; this can retain a looser fit until
rebuild. Meta sun VS selects the unbatched skin instance path, as local shadows
already do; default App batching is unchanged. The accepted grass culling without
bend is preserved.

## Screen UI

ScenePostUI shares one screen tail between full Append and ordinary AppendOpaque:
display transform, CaptureScreenFrame, window-resolution offscreen raster,
premultiplied ScreenComposite, then the existing PRESENT handoff. Empty captures
skip raster/composite. Slot-owned draw data, raster/composite owners and targets
live through the existing retirement; resize drains before replacing targets.
The existing backend records uploads and retains sampled resources. Meta reuses
unchanged font atlas generations on its ordered submission path; generic backend
callers retain their previous discard semantics. No steady-state wait is added.

Engine.UI's existing Settings panel, text, search and console-variable controls
are available in Release; F1 hides/shows them. RenderModule::BeginFrame generates
the UI. App::MsgProc handles UI first; WantsKeyboardInput and WantsMouseInput keep
camera polling, look and wheel input out of active UI. No new UI system, quality
search controller, world-space posters or full postprocessing path is introduced.
Particle motion output stays disabled by the preceding optimization.

## Verification

Release x64 build passed (exit 0; existing missing Jolt.pdb/LNK4099 warnings).
One ordinary no-argument launch passed: people and fox geometry visible, people
visibly change route positions and poses while the world remains paused, and
Engine.UI Settings draws over the scene. The existing search control filtered
App.Grass settings; focused keyboard input stayed in the field while the camera
view stayed fixed. Clearing the search restored the panel; F1 hid it. No console
setting was modified during this UI check. The current-run log contains no
ERROR/FATAL; its only warnings concern large original Bistro OBJ import budgets.
The steady operation list begins with skin palette uploads and skin deformation,
and ends with UI raster boundary, one screen composite and PRESENT. Existing
BistroCharacterChecks, SkinPlacementChecks and
OffscreenUITests were inspected as source context. No tests were added/expanded
or executed, per task/AGENTS verification contract. No accuracy comparison, matrix,
profiling, capture or long run is performed; no speedup is claimed. Alternate
quality modes, full diagnostic path and quantitative motion accuracy remain
source-reviewed only. The existing generic UI discard contract stays unchanged.
Independent correctness, standards, testing and performance source review found
no actionable findings. A separate simplification pass found no necessary change;
optional removal of an unused legacy field/getter was left outside this task.

Pre-existing particle optimization files, README particle hunk, SceneManagerModule,
BistroAdditionsTests, incremental migration plan, task file and Temp are preserved.
Only this task's README changes will enter the local commit; no push.
Motion Blur and other subsequent techniques remain unstarted.
Executable: Build/bin/x64/Release/App.MetaRenderGraph/App.MetaRenderGraph.exe.
Verification artifacts: Build/Verification/meta-bistro-animation-ui/.

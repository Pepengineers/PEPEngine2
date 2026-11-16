# Ordinary Meta categorized interior physics

Baseline: c1241257 on codex/bistro-rendering. User accepted the probe spheres and
requested this next stage. Existing unrelated painting/test/Temp/task changes
were preserved; no water solver optimization or offload was added.

## Ownership and quality

Reused BistroProps/BistroScene/PhysicsModule/Jolt and the original interior mesh.
The [source inventory](2026-11-17-bistro-interior-inventory.md) surveyed 1,133 OBJ
groups / 2,009 material sections. CookieJar glass/body groups merge into two
logical objects, leaving 974 movable objects and 157 excluded static groups.
Material sections of an object share a body. Strict name families avoid generic
`table` matches; unknown families remain static and are logged.

| Category | Objects | First dynamic quality |
| --- | ---: | --- |
| Glass | 256 | Low |
| Plates | 83 | Medium |
| Cutlery | 107 | High |
| Bottles | 304 | Epic |
| Furniture | 58 | Epic |
| Fabric | 36 | Epic |
| Baskets | 15 | Epic |
| Other movable | 115 | Epic |

| Quality | Dynamic / waves | Static inactive props | Static bodies incl room/walkway |
| --- | ---: | ---: | ---: |
| Off | 0 | 974 | 976 |
| Low | 256 | 718 | 720 |
| Medium | 339 | 635 | 637 |
| High | 446 | 528 | 530 |
| Epic | 974 | 0 | 2 |

Startup requests Categorized=true, Quality=1, SimulationEnabled=true, CountScale=1.
Readonly EffectiveCategorized/EffectiveQuality expose loaded ownership. Changes
require world recreation; F1 and existing App quality UI explain categories and
show restart requirement. Physics presets set only category/simulation/count
controls; other techniques keep their settings. Legacy App defaults to
Categorized=false and retains fraction/count-scale benchmarks until explicit
opt-in. New mode ignores both fraction and count multiplication.

The physics world factory is installed after the painting UI factory. It passes
only static architecture submeshes to that previous UI factory, then creates a
separate invisible room collider. Dynamic/inactive props are separate render
entities with their original mesh/material sections exactly once. No original
static-batch or collider duplicates remain. UI, particles, people, spheres and
exterior actors are not enrolled as dynamic props.

## Shapes and containment

Analytical cached shapes use authored group bounds/axes: cylinders for glass and
plates, capsules/cylinders for bottles, oriented boxes for cutlery/fabric and
small seat/back/leg compounds for furniture. Thin dimensions are clamped positive.
No new-mode dynamic triangle mesh or convex decomposition is used. The existing
physics backend caches compound geometry; shape building happens during loading.

The first launch failed on Paris_Flower_Pot_02A with Invalid shape dimensions.
BoxProxy now checks the PCA OBB output and uses a finite AABB proxy when the
orientation/extents are singular. This addresses invalid generated shape data;
confirmation on the failed asset at runtime remains pending because Computer Use
was stopped. Valid oriented proxy results retain their original behavior.

BuildRoom already closes all wall-chain openings around the rotated dining room
and narrow rear wing, with floor/ceiling/counter/step proxies. All movable group
centres are inside the surveyed polygon. Facade/rear walls remain invisible and
do not affect the free camera. Existing 30 cm walls, 36 cm floor slabs, CCD and
four collision substeps are retained for the existing 3000 cm/s cap. No periodic
teleport or containment reset is introduced. Some Epic proxies may start near
walls/floor; source survey and a short smoke cannot prove long-term containment.

## Time, dirty data and reflections

Ordinary Meta unpauses the world and reuses App's single 60 Hz accumulator.
Existing maximum eight ticks/frame and wall-time catch-up limit remain, and
unprocessed time remains debt. Physics is not stepped a second time. Existing
BistroWaves BeforePhysicsStep applies lift then scatter, registering only dynamic
categories; seed 12345, initial 5..10 second wait, lift 500 cm/s, scatter 2100 cm/s
after 0.3 seconds and cap 3000 cm/s are unchanged. Phase-change logs include cycle
and participant count. World destruction clears the callback/factory before
entity teardown and participant-ID reuse.

Characters, skinning, grass, water and particles use the existing time paths once,
now respecting pause/time scale. Sunlight uses the same world-scaled CPU delta;
UI/cloud picture render-time animation remains independent as previously designed.
DynamicBodyUpdate writes only changed poses. GPUData publishes previous-world
once per displayed frame after all fixed substeps, preserving motion vectors.
ReflectionDynamic is body ownership and remains true while sleeping; static bake
has no dynamic duplicates. The probe snapshot reads the shared non-destructive
dirty journal before shadow preparation consumes it. Quality/reload changes
membership and cache topology. Preview spheres keep fixed original anchors and
never receive physics bodies or follow furniture.

## Verification and handoff

- Release build passed before the ordinary attempt; final source fix is rebuilt.
- Imported runtime inventory matched the source categories and requested/effective
  Low=1. The launch then failed at the flower-pot collider before a presented wave.
- Source simplification removed redundant legacy selection/sorting in category mode
  and an unused reload helper. Source review retains trust-boundary validation.
- The user stopped Computer Use with physical Escape. No further computer-use or
  alternate runtime launch was attempted in this turn. Final runtime, clean exit,
  actual flight/collisions and preservation of effects on a presented frame remain
  NOT VERIFIED. This stage is not visually accepted yet.
- No tests added, mode matrix, accuracy experiments, profiling or long soak run,
  per task/AGENTS. Other presets are source/inventory reviewed only.

Executable: Build/bin/x64/Release/App.MetaRenderGraph/App.MetaRenderGraph.exe.
Build log: Build/meta-bistro-interior-physics-build.log.
Stop for the remaining Low-wave smoke and user FPS/CPU/GPU acceptance; no FG,
general offload or final App/Meta comparison is started. Local commit only, no push.

## Subsequent user acceptance

2026-11-17: the user confirmed the glassware flies and the physics works. This
supersedes the pending flight acceptance above. Requested follow-up improves
visibility with separate glass materials (opacity 0.85 plus subtle cyan emission)
and speeds the cloud picture up by four through Meta.CloudPicture.SpeedScale.
These visual adjustments passed Release build; appearance was not checked again.

# Frozen Bistro reflection captures

Integrate four local box-projected cubemap probes covering the entrance, street
and interior. Use the existing reflection-probe pipeline and choose positions
inside free space. Default `App.ReflectionProbes.Count=4`; zero removes probes,
runtime changes reconcile the count, maximum is the renderer's probe limit.

Own `Apps/App/src/BistroReflections.h`, focused reflection checks and minimal
reflection-renderer changes required for true frozen captures. Coordinate exact
test paths before editing shared tests. Public interface is in README.md.

Existing Implementation=1 caches static geometry but keeps rendering dynamic
overlays. That alone does not satisfy this task. Bake static scene/environment
once, publish all six filtered faces, then stop capture/filter work until explicit
invalidation, changed capture settings or scene replacement. Exclude moving props,
NPCs, particles and water from the frozen captures. Preserve current dynamic and
cached-static modes for other callers. Baking during initial scene warmup is
acceptable; a disk cache or offline baking tool is outside scope.

Inspect `GDX12ReflectionProbePass`, `GDX12ReflectionProbes`,
`ReflectionProbeComponent` and `ReflectionProbeTests` before editing. Verify first
publication, frozen steady-state counters over moving frames, explicit rebake,
default/zero/count changes, and existing reflection regression tests. Report
changed paths, exact checks/results and any limitations. Leave commits to the
coordinator.

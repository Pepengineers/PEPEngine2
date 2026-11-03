# Bistro scene additions

Integrate existing rendering techniques into the App's Bistro scene. Preserve the
current geometry, 346 moving props, eight particle effects, camera and lighting.
Scope is integration and proof that each feature works; visual polishing and
performance tuning are separate work.

| Task | Default | Application CVar |
|---|---:|---|
| [Frozen reflection captures](reflections.md) | 4 probes | `App.ReflectionProbes.Count` |
| [Entrance grass](grass.md) | 1 patch | `App.Grass.Count` |
| [Water](water.md) | 1 container | `App.Water.Count` |
| [Street NPCs and interior fox](characters.md) | 4 people, 1 fox | `App.NPC.Count`, `App.Fox.Count` |

Counts are validated runtime integers; zero removes that feature. Increasing a
count creates independent objects with sensible spacing, up to a documented
resource limit. Register these CVars only in the Bistro application and its tests.
Use the existing INI/console mechanism. Respect pause and world time scale.

## Integration contract

Each feature owns a header under `Apps/App/src`, containing its namespace and a
`Scene` class with `void Update(World&, float deltaSeconds)`. A first update with
zero delta creates the default objects. Subsequent updates reconcile count
changes and perform any application-owned movement. Each Scene belongs to one
App/world; avoid global entity ownership and duplicated renderer implementations.

The coordinator owns App.cpp hooks, project registration, the combined startup
test, shared documentation and commits. Feature workers own only their task's
files. Existing dirty files belong to other work: preserve their content and
isolate any coordinator changes. Workers leave the index untouched. Shared builds
and GPU tests run under coordinator scheduling, one at a time.

Acceptance: Release/x64 App and focused tests build; default and zero/changed counts
are verified; real Bistro renders with all additions and no D3D12 debug-layer
errors. Reflection counters prove no capture/filter after initial publication.
Character checks prove both world movement and changing skeletal poses. Grass and
water checks prove active GPU work and visible placement, not just ECS presence.

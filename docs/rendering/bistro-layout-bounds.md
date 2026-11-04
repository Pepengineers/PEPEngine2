# Bistro layout bounds

The App copies effect bounds once after creating the scene additions. These are
independent world-axis-aligned layout boxes, visible by default. Editing them never
changes ECS transforms, particle settings, water simulation, grass, or animation.
Moving characters leave their snapshot boxes in place. Changing real effect counts
does not add or delete layout boxes until the next App startup.

Open F1 settings and filter by `App.Bounds` or an object's name. Each
`App.Bounds.<name>.` group exposes `CenterX/Y/Z` in world centimetres,
`HalfExtentX/Y/Z` in centimetres, and `Visible`. Full size is twice the half extent.
`App.Bounds.Show` toggles all boxes. Values are session-only; these dynamic controls
are created after startup config loading and are not Engine.ini settings.

| Colour | Names at default counts | Initial bounds |
| --- | --- | --- |
| Yellow | `BistroEntranceFountain`, `BistroBlueFountain`, `BistroTerraceEmbers`, `BistroPlanterFireflies`, `BistroStreetFirebursts`, `BistroInteriorDust`, `BistroTableSteam`, `BistroSparkRain` | Emitter spawn shape, or confinement volume if enabled at startup |
| Green | `BistroGrass0` | Grass culling bounds, including blade height and maximum bend |
| Blue | `BistroWater0` | Fluid simulation container; excludes the stone basin |
| Pink | `BistroNPC0` through `BistroNPC3`, `BistroFox0` | CPU-skinned mesh vertices at the initial pose and world transform |

Particle spawn boxes do not enclose the later particle trajectories. Character
boxes are pose snapshots, not walking-route or animation-cycle envelopes. Boxes
are drawn through geometry as an overlay, using the existing near-plane clipping.
F1 hides the settings window; `App.Bounds.Show` controls the boxes separately.

Dust and Steam use independent layout presets instead of their spawn snapshots:

- Dust: one approximate box covering the Bistro interior, centre (559,221,-348),
  half extents (694,181,779) cm, rotated -28.5 degrees around Y to match Steam.
  Some corners extend outside the walls by design.
- Steam: one simple box along the behind-bar space, centre (922,227,-293),
  half extents (305,175,85.5) cm, restored to its original -28.5-degree Y rotation
  along the bar. Dust shares the same rotation, so their faces are parallel.

These presets do not change either emitter. Centres are world-space; half extents
follow the rotated box axes. Particle confinement will be connected only after
the layout is approved.

The regression checks run in `BistroAdditionsTests`: control availability, source
object isolation, extent validation, paused drawing, visibility, and persistence
across scene updates. `BistroStartupTests` disables the boxes to isolate its
existing settings-window checks.
Use `BistroAdditionsTests.exe --bounds-only` to run the layout checks and normal
shutdown without the remaining effect checks.

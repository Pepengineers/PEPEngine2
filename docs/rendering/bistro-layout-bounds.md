# Bistro layout bounds

The App creates layout boxes after creating the scene additions, visible by default.
Dust and Steam are linked to their boxes: editing their centres or half extents
updates their emitter transforms, box spawn and confinement volumes. Other boxes
remain independent snapshots; editing them does not change their scene objects.
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

Unlinked particle spawn boxes do not enclose the later particle trajectories. Character
boxes are pose snapshots, not walking-route or animation-cycle envelopes. Boxes
are drawn through geometry as an overlay, using the existing near-plane clipping.
F1 hides the settings window; `App.Bounds.Show` controls the boxes separately.

Dust and Steam use the approved layout presets instead of their old spawn snapshots:

- Dust: one approximate box covering the Bistro interior, centre (559,221,-348),
  half extents (694,181,779) cm, rotated -28.5 degrees around Y to match Steam.
  Some corners extend outside the walls by design.
- Steam: one simple box along the behind-bar space, centre (922,227,-293),
  half extents (305,175,85.5) cm, restored to its original -28.5-degree Y rotation
  along the bar. Dust shares the same rotation, so their faces are parallel.

Centres are world-space; half extents follow the rotated box axes. Both emitters
spawn uniformly throughout the box and use the existing reflecting GPU volume to
stay inside. Particle centres leave a margin equal to the largest billboard's
half-diagonal, keeping its corners inside the visible bounds as well.
Moving/resizing a linked box resets its pool so old particles do not remain outside;
unchanged boxes do not reset simulation. Visibility controls only the overlay.
`App.Particles.Volume.*` overrides still apply to the other six emitters; the
approved Dust/Steam bounds take precedence for these two. Budget, simulation mode,
pause and time controls continue to apply to all emitters.

The regression checks run in `BistroAdditionsTests`: control availability, source
unlinked object isolation, linked spawn/confinement settings, extent validation,
paused drawing, visibility, and persistence
across scene updates. `BistroStartupTests` disables the boxes to isolate its
existing settings-window checks.
Use `BistroAdditionsTests.exe --bounds-only` to run the layout checks and normal
shutdown without the remaining effect checks.

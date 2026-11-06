# Bistro grass controls

These application-owned runtime CVars appear in the settings window (search for
`App.Grass.`), and can be set in `[ConsoleVariables]` in the App executable's
own `Config/Engine.ini`. Keep them out of other executables' shared INI files.
Dimensions and distances are centimetres. Settings apply to every existing patch;
count changes retain the surviving patches and their renderer-owned material.

## Grass

| CVar after `App.Grass.` | Default | Accepted values / meaning |
|---|---:|---|
| `Count` | 1 | 0..16 independent patches; 0 removes grass and owned wind fields |
| `ExtentsXCm` | 200 | 1..100000, local X half-width |
| `ExtentsZCm` | 530 | 1..100000, local Z half-width |
| `DensityPerSquareMetre` | 1400 | 0..100000, blades per square metre; 0 draws no blades |
| `MaxBlades` | 65536 | 0..262144 per-patch budget; 0 inherits renderer quality |
| `Implementation` | 0 | 0 Ribbon, 1 CrossedRibbons |
| `Segments` | 4 | 0..8 segments per blade; 0 inherits renderer quality |
| `Seed` | 7919 | Unsigned 32-bit root seed; patch index multiplies it |
| `HeightCm` | 40 | 0.1..10000 |
| `HeightVariation` | 0.25 | 0..1 relative random height variation |
| `WidthCm` | 3 | 0.01..1000 |
| `BendSeconds` | 0.15 | 0..10, wind response time |
| `MaxBendCm` | 20 | 0..10000, bend cap and conservative bounds expansion |
| `LodDistanceCm` | 1500 | 1..1000000, near LOD; far-card/cull distances are 2x/4x |
| `CastShadows` | true | Directional and local shadow casting |

The actual blade count is
`min(ceil(4 * ExtentsXCm * ExtentsZCm / 10000 * DensityPerSquareMetre), resolved MaxBlades)`.
Defaults retain 59360 blades over 42.4 square metres beneath the building_02 arch. Changing the area therefore changes density-driven
counts until the budget is reached. The count is not an allocation-only reserve.
Grass culling uses the existing `GrassLocalBounds`, including height variation,
width and maximum bend. Wind coverage uses the rotated horizontal bounds in world XZ,
with a minimum half-size of 90x70 cm.

## Wind

| CVar after `App.Grass.Wind.` | Default | Accepted values / meaning |
|---|---:|---|
| `Enabled` | true | Enable each patch's field |
| `Algorithm` | 0 | 0 Analytic directional gusts, 1 ProjectedGrid pressure solver |
| `SpeedCmPerSecond` | 100 | 0..500 base flow speed |
| `AngleDegrees` | 67.5 | -180..180, world XZ; 0 means +X, 90 means +Z |
| `GustAmplitude` | 0.6 | 0..1 relative gust amplitude |
| `GustFrequencyHz` | 0.4 | 0..100 |
| `GustWavelengthCm` | 500 | 1..1000000 |
| `ForcePerSecond` | 2 | 0..100 projected-grid forcing |
| `DampingPerSecond` | 0.2 | 0..100 projected-grid velocity damping |
| `TimeScale` | 1 | 0..10; 0 stops wind time |
| `MaxStepSeconds` | 1/60 | 0.0001..0.1 projected-grid step limit |
| `MaxSubsteps` | 4 | 1..16 steps per frame |
| `GridResolution` | 0 | 0 inherits renderer quality, otherwise 8..256 |
| `PressureIterations` | 0 | 0 inherits renderer quality, otherwise 1..128 |

Finite values and ranges are validated before CVar writes succeed. Geometry and
algorithm are typed enums so the existing UI discovers their dropdown choices.
Controls are compared with the previous settings each update: unchanged frames
do not reset grass history or wind state. Changes affecting wind reset the wind
simulation and grass deformation history; grass-only edits preserve wind unless
they change its required coverage. The existing renderer applies world pause and
world time scale.

## MGPU mapping

The reference is the grass UI in `MGPU-Grass/HybridGrassApp.cpp` in the user's MGPU
checkout. Field world size/scale maps to X/Z half-extents; blade count/density maps
to area, density and the per-patch budget. Blade size and base segments map to
height/width/variation and segments. Base-flow angle/strength and wind
amplitude/intensity map to angle, speed and gust controls. The 2D fluid toggle,
grid, pressure iterations, forcing, dissipation and time-step controls map to
ProjectedGrid and its existing solver controls. This renderer uses a damping rate
per second, rather than MGPU's per-step dissipation multiplier.

The existing PEPEngine renderer has ribbon/crossed-ribbon geometry and
analytic/projected-grid wind. MGPU's alternate GS/multi-adapter paths, per-LOD
tufts, cursor injection, obstacle solver, vorticity and SDOF-specific controls are
not represented by inactive or approximate toggles. Local reflections remain the
scene's frozen static captures, which exclude moving grass.

The default field is centred at `(1399.6, 23.3, 1369)` cm and rotated 69 degrees
about Y, sharing the confined firefly swarm footprint. Four-segment tapered ribbons
and stronger directional gusts match the compute grass silhouette shown in the
GraphiCon grass article. Both geometry choices expand in compute and draw indexed
indirect; no geometry-shader pipeline is present. Existing saved INI values still
override these defaults. Extra count-controlled fields are spaced by their extents.

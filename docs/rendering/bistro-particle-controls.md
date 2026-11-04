# Bistro particle controls

These application-only runtime CVars appear in the settings UI (search for
`App.Particles.`). They can also be set in `[ConsoleVariables]` in the App
executable's own `Config/Engine.ini`. Keep them out of other applications' INIs.

| CVar | Default | Meaning |
|---|---:|---|
| `App.Particles.Count` | -1 | Exact total live simulation budget, 0..4194304; -1 uses `CountScale` |
| `App.Particles.CountScale` | 1 | Existing authored 250000 budget multiplier; <=0 retains the existing fallback to 1 |
| `App.Particles.Simulation` | 0 | 0 analytic drag/trapezoidal position; 1 MGPU-style semi-implicit Euler |
| `App.Particles.Volume.Enabled` | false | Override all eight emitters with a bounded box and box spawn |
| `App.Particles.Volume.HalfExtentX` | 100 | Emitter-local box half-width in cm, 0.01..1000000 |
| `App.Particles.Volume.HalfExtentY` | 100 | Emitter-local box half-height in cm, 0.01..1000000 |
| `App.Particles.Volume.HalfExtentZ` | 100 | Emitter-local box half-depth in cm, 0.01..1000000 |

`Count=0` disables all eight effect components. Positive counts are distributed
with the existing 4:3:2:2:2:1:1:1 weights and exact accumulated rounding. The
maximum keeps each pool at or below its existing 1048576-particle limit. Global
`Render.Particles.Enabled` remains available independently.

With the volume override off, every effect retains its authored spawn shape and
dimensions. Turning it on fills the selected box at spawn and confines particle
centres to that box for their whole life. The box follows the emitter transform,
including rotation/scale. A boundary clamps the centre and reflects its incoming
local velocity using the emitter's existing collision restitution. This is a
simulation domain, not just spawn dimensions; sprite/mesh size can extend beyond
the centre bounds. A boundary hit discards the remaining travel in that substep;
very small boxes/high speeds need more substeps or a future swept multi-hit
solver for accurate bounce timing. Existing screen-depth collision remains independent.

Count/spawn/domain changes reset the affected live pools and histories; exclude
that frame from timing. Integrator changes preserve live particles and velocities.
Unchanged controls do not reset pools. Disabling the volume restores each authored
shape/dimensions. The existing firefly geometry, atlas, colors, depth collision and
pause/world time-scale path remain in use.

The algorithm selection is literal. The original MGPU
`MGPU-Particles/Shaders/ComputeParticle.hlsl` advances velocity with force and then
advances position with the new velocity (semi-implicit Euler). Its shared shader
uses the same algorithm; no vortex/curl algorithm family was found. Mode 1 uses
that integrator with PEPEngine's existing acceleration and wind/drag forces. Mode 0
preserves the existing analytic drag velocity update and trapezoidal position.
Blending and spawn shape are not presented as simulation algorithms.

`BistroParticleControlsChecks.h` checks budget/default compatibility, invalid
input, runtime propagation and stable pool revisions. The GPU fixture in
`ParticleSimulationControlsChecks.h` compares actual particle motion readback to
independent .5 cm/1 cm displacement oracles and to a rotated one-centimetre volume
with reflected velocity and pause. Build/run these through the coordinator's
existing App and SecondGroup test projects; no additional engine is needed.

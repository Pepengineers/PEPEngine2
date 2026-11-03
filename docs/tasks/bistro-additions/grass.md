# Bistro entrance grass

Integrate one small grass patch next to the entrance, at pavement/planter height,
outside the doorway and walking route. Reuse `GrassRenderComponent`, the existing
GPU grass renderer and material creation. Add a wind field only if needed to
demonstrate the existing grass wind support; it is owned with the patch.

Own `Apps/App/src/BistroGrass.h` and a dedicated focused check header/source.
Public interface is in README.md. Default `App.Grass.Count=1`; zero removes all
owned patches and wind entities. Runtime count changes create/remove independent
patches with sensible spacing; choose and document a modest validated upper bound.
Coordinate before changing renderer code or shared project/test files.

Inspect grass fixtures in `SecondGroupTests` and existing creation/material APIs.
Verify exact default/zero/changed counts, valid materials/bounds, pause behavior,
and an active real GPU grass draw at the Bistro entrance. Scene presence alone is
insufficient. Report changed paths, checks/results and limitations. Leave shared
builds, GPU test scheduling, App.cpp hooks and commits to the coordinator.

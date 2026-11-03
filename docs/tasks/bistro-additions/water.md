# Bistro water

Integrate one compact bounded water container outside Bistro near the entrance,
without blocking the doorway or overlapping the grass. Reuse the existing SPH
solver, surface renderer and water depth/motion integration. Keep the initial
particle budget small and the simulation contained. Reuse scene geometry for a
basin when practical; otherwise add only the minimal visible container needed.

Own `Apps/App/src/BistroWater.h` and a dedicated focused check header/source.
Public interface is in README.md. Default `App.Water.Count=1`; zero removes owned
water/container entities. Runtime changes create/remove independent containers;
choose and document a modest validated upper bound. Coordinate before modifying
renderer code or shared project/test files.

Inspect fluid fixtures in `SecondGroupTests` and component validation. Verify
default/zero/changed counts, finite contained simulation and actual water pixels
or depth in Bistro, plus pause/time scale. Report changed paths, checks/results
and limitations. Leave shared builds, GPU scheduling, App.cpp hooks and commits to
the coordinator.

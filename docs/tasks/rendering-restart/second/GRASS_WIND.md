# Grass renderer и поле ветра

Статус: ПОДГОТОВЛЕНО, НЕ ЗАПУСКАТЬ до отдельного указания пользователя.

Общий контракт: ../COMMON.md.

Настраиваемый grass render component и отдельный wind field/component. Донор SingleExpanded MGPU-Grass/ComputeGrass + Common/WindFluidSimulator: compute-expanded blades, compact GPU geometry/indirect draw, LOD/far cards/culling; 2D advection/divergence/Jacobi/projection wind, честная художественная модель, не выдавать за полноценную физику стебля. Включить main/shadows/reflection captures/motion и conservative caster bounds: off-camera caster не исчезает из тени. CVars render/simulation Enabled, Quality, density/shape/bend/LOD/max-distance/wind force/direction/grid/iterations, реальные implementation choices только если существуют. Paused dt/owner reuse/previous positions/cached shadows и resource lifetimes обязательны. Пустые компоненты не создаются, пустая сцена не выделяет arenas. GPU compact-count/индексная геометрия/motion/shadow readbacks, settings, reset и timings.

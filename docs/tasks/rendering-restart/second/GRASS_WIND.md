# Grass renderer и поле ветра

Статус: запуск поручен центральному проверяющему ПОСЛЕ завершения проверки и исправлений первой группы. Сейчас не запускать.

Общий контракт: ../COMMON.md.

Настраиваемый grass render component и отдельный wind field/component. Донор SingleExpanded MGPU-Grass/ComputeGrass + Common/WindFluidSimulator: compute-expanded blades, compact GPU geometry/indirect draw, LOD/far cards/culling; 2D advection/divergence/Jacobi/projection wind, честная художественная модель, не выдавать за полноценную физику стебля. Включить main/shadows/reflection captures/motion и conservative caster bounds: off-camera caster не исчезает из тени. Глобальные CVar только Enabled и Quality для grass/wind. Render/simulation controls, density/shape/bend/LOD/max-distance/wind force/direction/grid/iterations и реальный implementation choice — поля соответствующих компонентов. Paused dt/owner reuse/previous positions/cached shadows и resource lifetimes обязательны. Пустые компоненты не создаются, пустая сцена не выделяет arenas. GPU compact-count/индексная геометрия/motion/shadow readbacks, settings, reset и timings.

Авторы этой задачи сейчас добавляют код без сборок/тестов/benchmarks и отдельной ревизии. Приведённые проверки — критерии последующей централизованной проверки. Соблюдать code style и ponytail при написании кода.

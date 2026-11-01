# Полностью GPU particles

Статус: запуск поручен центральному проверяющему ПОСЛЕ завершения проверки и исправлений первой группы. Сейчас не запускать.

Общий контракт: ../COMMON.md.

Высококачественный GPU particle component с GPU emitters: GPU spawn/lifetime/simulation/dead-alive compaction/culling/опциональная GPU sorting/indirect draw; CPU задаёт author parameters, не читает counts обратно для продолжения цикла. Основы AMD GPUParticles11 MIT и FidelityFX Parallel Sort kernels; реальный GPU-generated count/args (не CPU host sort count). Быстрые billboards, meaningful render quality, soft intersections и нужные blend modes/материалы; сортировать между эмиттерами, если обещается общий sorted alpha. Корректные counters/CAS/no underflow/UAV dependencies, reset/reuse/pause. GPU event child/subemitters отложены. Глобальные CVar только Enabled и Quality. Simulation/render controls, sorting, emission/speed/size/lifetime/forces/render scale и все прочие параметры — в ParticleEmitterComponent, без глобальных CVar-дублей. Additive energy не класть в нормализуемую WBOIT accumulation с alpha0. Не совмещать writable DSV и sampling того же depth. Проверки pool zero/full/death/reuse/count boundaries/multiset и реального indirect draw, cross-emitter overlap, soft depth и timings. Не добавлять emitters в Bistro.

Авторы этой задачи сейчас добавляют код без сборок/тестов/benchmarks и отдельной ревизии. Приведённые проверки — критерии последующей централизованной проверки. Соблюдать code style и ponytail при написании кода.

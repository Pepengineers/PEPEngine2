# Полностью GPU particles

Статус: ПОДГОТОВЛЕНО, НЕ ЗАПУСКАТЬ до отдельного указания пользователя.

Общий контракт: ../COMMON.md.

Высококачественный GPU particle component с GPU emitters: GPU spawn/lifetime/simulation/dead-alive compaction/culling/опциональная GPU sorting/indirect draw; CPU задаёт author parameters, не читает counts обратно для продолжения цикла. Основы AMD GPUParticles11 MIT и FidelityFX Parallel Sort kernels; реальный GPU-generated count/args (не CPU host sort count). Быстрые billboards, meaningful render quality, soft intersections и нужные blend modes/материалы; сортировать между эмиттерами, если обещается общий sorted alpha. Корректные counters/CAS/no underflow/UAV dependencies, reset/reuse/pause. GPU event child/subemitters отложены. CVars simulation/render enable, Quality, sorting, emission/speed/size/lifetime/forces/render scale и реальные params без перетирания author data. Additive energy не класть в нормализуемую WBOIT accumulation с alpha0. Не совмещать writable DSV и sampling того же depth. Проверки pool zero/full/death/reuse/count boundaries/multiset и реального indirect draw, cross-emitter overlap, soft depth и timings. Не добавлять emitters в Bistro.

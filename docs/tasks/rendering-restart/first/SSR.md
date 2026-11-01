# AMD FidelityFX SSSR и Reflections Denoiser

Статус: запуск отдельного чата Sol / high разрешён.

Сначала прочитай ../COMMON.md (относительный путь из first: `../COMMON.md`).

Реализуй готовые AMD FidelityFX SSSR + Reflections Denoiser с DX12 backend. Пользователь выбрал этот путь, не старый SSR и не копирование Unity HDRP. SDK v1.1.4 pin c6efa6bf7f2027b3ec94f28578bb5965eabb9e55 — выбранная воспроизводимая база; без Cauldron. Официальные источники: https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK , https://gpuopen.com/fidelityfx-sssr/ . Сохрани MIT notices и минимальную воспроизводимую сборку необходимых компонентов.

SSSR читает linear HDR, reverse-Z depth, правильные motion vectors, world normals, roughness/material mask, environment cubemap и DFG. Обеспечь единственный specular resolve: SSR заменяет соответствующий непрямой specular вклад, а miss использует IBL/probe fallback без повторного сложения энергии. На этой независимой ветке probes ещё отсутствуют: поддержи корректный global fallback и документируй конкретный seam локальной пробы для последующего объединения. Не строй fake интерфейс без потребителя.

Проверь требования SDK к jittered previous projection и motion convention; не меняй общий motion buffer, если конверсия нужна только SDK. Контекст/история сбрасываются при cut, resize, re-enable и настоящей смене source identity. Корректно восстанавливай кеш native command-list state после стороннего SDK dispatch. HDR composition/tone mapping и XeSS исправляй только в объёме, нужном для SSR.

CVars: Enabled, Quality Off/Low/Mid/High, реальные trace/roughness/depth/thickness/temporal/rays и denoiser параметры, где SDK поддерживает изменение. Не делай fake Implementation selector для единственного SSSR.

Проверки: GPU ray-hit с отличимым цветным отражением, roughness/cutoff, miss fallback, отсутствие двойного specular, warm/cold history, stationary jitter и движение камеры/объекта, cut/resize/Off/re-enable, Odd dimensions/frame slots, реальные стеклянные бутылки Bistro без новых объектов, GPU states, одинаковые workload timings/memory. Alpha выходного denoiser не объявляй confidence без подтверждения исходником.

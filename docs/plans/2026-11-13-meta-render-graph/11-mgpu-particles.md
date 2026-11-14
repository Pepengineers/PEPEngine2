---
title: "U11 Particle groups and shared batch transfer - Plan"
type: refactor
date: 2026-11-13
---

# U11 Particle groups and shared batch transfer - Plan

## Contract

Выполнить только U11 из [мета-задания](00-meta-plan.md), R3, R7–R14; KTD9–KTD11. Требуется принятый U10, particle single-GPU U6/U8 и актуальный результат production particle-offload чата. Если последний ещё не завершён, не редактировать его незавершённые файлы; использовать принятый local solver и согласованную границу reusable части, указав отсутствие готового reference.

Ограничения пользователя также зафиксированы в [первом задании частиц](../2026-11-13-particle-simulation-mgpu.md); это не разрешение повторно реализовать отдельный production offload runtime.

## Outcome

Эксперимент исполняет одновременно локальные DIRECT-эмиттеры, локальные COMPUTE-эмиттеры и отмеченные secondary-эмиттеры. Все отображаются на основной карте. Вторичная группа передаёт общий payload независимо от числа эмиттеров; совместная MGPU-вода продолжает работать.

## Files

Experimental particle adapters, shared publication/producer APIs U9, scene controls нового приложения. Переиспользовать актуальные `ParticleEmitterComponent.h`, `GDX12Particles.h`, `GDX12ParticleSimulation.cpp`, `GDX12ParticlesPass.cpp`, `GDX12ParticleOffload.*` и `ParticleTransfer.hlsl`, если они к этому моменту приняты; не плодить вторую отметку эмиттера с тем же смыслом.

Пути fixtures как ориентиры (создание и расширение не требуются): `Apps/App.MetaRenderGraph/tests/ParticleOffloadGpuTests.cpp`, `MixedEmitterSceneTests.cpp`, `CombinedMgpuSceneTests.cpp`. Отчёт `docs/reports/meta-render-graph/11-mgpu-particles.md`.

## Approach

1. Централизованно определить requested placement и eligible placement: отметка + DepthCollision=false допускают secondary, DepthCollision=true остаётся на primary с действующими коллизиями. Soft particles при рисовании переносу не мешают. Все допустимые отмеченные эмиттеры участвуют, без эвристического выбора подмножества.
2. Secondary particle state хранится постоянно, inputs/events привязаны к шагам. Передать реальный interaction field там, где он нужен, один раз для общих потребителей; depth texture не экспортировать. Scene field — самостоятельная версия входа с lifetime и зависимостями, не замена пустыми данными.
3. Использовать device/queue/submission ownership нового executor; не создавать отдельный device/worker/shared ring на каждый emitter и не дублировать уже выбранную secondary device воды. Вода и частицы могут иметь независимые операции/публикации на одном scheduler.
4. Упаковать render-required данные всех secondary emitters с IDs/generations/counts/offsets в один buffer. Экспорт и импорт каждого выбранного particle batch — по одной contiguous copy на сторону; упаковочные dispatch могут быть отдельными. N CopyBufferRegion внутри одного list не выполняет R13. Без синхронного CPU readback count ради каждой копии.
5. Отрисовка объединяет local/imported inputs, сохраняет общую SortedAlpha сортировку и WBOIT semantics. История связывается с предыдущим показанным состоянием и идентичностью живой частицы, не с переиспользованным pool index умершей.
6. При изменении DepthCollision/отметки завершить использование старого размещения и не выполнить шаг дважды. Сохранить существующий принятый контракт миграции/reset; если он отсутствует, явно описать reset на смене размещения, не отключать depth collision молча. Изменение состава требует новой generation метаданных и защиты старых результатов.
7. Проверить работу совместно с водой без обязательного water+particles batch barrier. Не ставить основной frame за будущим secondary результатом, когда выбран LatestCompleted.

## Smoke-check

Применяется [общий порядок проверки](00-meta-plan.md#verification-contract). Перенесённые частицы и вода рассчитываются на второй карте и отображаются в общей сцене. Достаточно обычного короткого запуска без явных ошибок; уже полученный результат можно использовать повторно без нового прогона.

## Acceptance and Handoff

Указать размещение эмиттеров и общий путь передачи particle payload. Кратко записать commit, выбранный режим и итог: работает / не работает / не запускалось, с причиной при наличии. Локальный коммит только своих изменений. Следующее задание: [U12](12-acceptance.md); переходить только в разрешённых пользователем пределах.

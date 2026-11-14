---
title: "U8 Async queues on one GPU - Plan"
type: feat
date: 2026-11-13
---

# U8 Async queues on one GPU - Plan

## Contract

Выполнить только U8 из [мета-задания](00-meta-plan.md), R5–R10; KTD2–KTD8. Требуется принятый U7. Только одна карта. Нельзя переходить к второму устройству, пока same-GPU queues не прошли этот этап.

## Outcome

Один логический граф компилируется в DIRECT/COMPUTE/COPY-пакеты с поздними ожиданиями. Параллельная запись U3 сохраняется. Выбор размещения явный; нет автоматического scheduler по целевым FPS.

## Files

Изменить Graph compiler/executor/recorder в App.MetaRenderGraph; добавить `src/Graph/QueueSchedule.*` при необходимости. Shared device/queue wrappers расширять минимально; изучить текущие `GDX12Device::TryGetComputeQueue`, queue ownership и их ограничения. Адаптировать выбранные Techniques из U4–U7.

Пути fixtures как ориентиры (создание и расширение не требуются): `Apps/App.MetaRenderGraph/tests/QueueScheduleTests.cpp`, `AsyncQueuesGpuTests.cpp`, расширения `ParticlesGpuTests.cpp` и `SingleGpuSceneTests.cpp`. Отчёт `docs/reports/meta-render-graph/08-async-single-gpu.md`.

## Approach

1. U8.1: включить фактические COMPUTE/COPY queues, command-list types и per-queue completion. Сначала fixture upload COPY → compute transform COMPUTE → DIRECT draw, с независимым DIRECT-префиксом до первого потребителя. Plan dump показывает Wait/Signal и отсутствие лишнего раннего wait.
2. U8.2: реализовать fork/join, несколько производителей и последний читатель ресурса на разных очередях. Барьеры ставятся на очереди, которая вправе их выполнить. Учесть COMMON/promotion/decay, read-state union и UAV hazards. Не переносить предположения старого single-thread tracker.
3. U8.3: подключить подходящие реальные compute участки: water solver, particle groups, grass preparation или AO по их inputs. Сделать явное разделение эмиттеров: одна группа DIRECT, другая COMPUTE на этой же карте; DepthCollision допускается, если локальный depth producer заявлен зависимостью. Это не MGPU-исключение из U11.
4. U8.4: decomposed render consumer. Независимая подготовка/допустимые draws группы A идут до wait группы B; для SortedAlpha собирается общий набор и сохраняется общая сортировка, для WBOIT сохраняется единый resolve. Не объявлять корректным произвольный draw-by-queue order.
5. U8.5: перенести подходящие uploads на COPY. Отправлять готовые пакеты без обязательного ожидания записи всей сцены. Реальное reuse каждого resource/allocator slot учитывает все очереди, которые его использовали.

## Smoke-check

Применяется [общий порядок проверки](00-meta-plan.md#verification-contract). Сцена запускается с DIRECT/COMPUTE/COPY и выдаёт GPU-результат. Достаточно обычного короткого запуска без явных ошибок; уже полученный результат можно использовать повторно без нового прогона.

## Acceptance and Handoff

Указать выбранные очереди и границы packets. Кратко записать commit, выбранный режим и итог: работает / не работает / не запускалось, с причиной при наличии. Локальный коммит только своих изменений. Следующее задание: [U9](09-cross-adapter-transport.md); переходить только в разрешённых пользователем пределах.

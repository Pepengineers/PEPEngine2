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

Тесты: `Apps/App.MetaRenderGraph/tests/QueueScheduleTests.cpp`, `AsyncQueuesGpuTests.cpp`, расширения `ParticlesGpuTests.cpp` и `SingleGpuSceneTests.cpp`. Отчёт `docs/reports/meta-render-graph/08-async-single-gpu.md`.

## Approach

1. U8.1: включить фактические COMPUTE/COPY queues, command-list types и per-queue completion. Сначала fixture upload COPY → compute transform COMPUTE → DIRECT draw, с независимым DIRECT-префиксом до первого потребителя. Plan dump показывает Wait/Signal и отсутствие лишнего раннего wait.
2. U8.2: реализовать fork/join, несколько производителей и последний читатель ресурса на разных очередях. Барьеры ставятся на очереди, которая вправе их выполнить. Учесть COMMON/promotion/decay, read-state union и UAV hazards. Не переносить предположения старого single-thread tracker.
3. U8.3: подключить подходящие реальные compute участки: water solver, particle groups, grass preparation или AO по их inputs. Сделать явное разделение эмиттеров: одна группа DIRECT, другая COMPUTE на этой же карте; DepthCollision допускается, если локальный depth producer заявлен зависимостью. Это не MGPU-исключение из U11.
4. U8.4: decomposed render consumer. Независимая подготовка/допустимые draws группы A идут до wait группы B; для SortedAlpha собирается общий набор и сохраняется общая сортировка, для WBOIT сохраняется единый resolve. Не объявлять корректным произвольный draw-by-queue order.
5. U8.5: перенести подходящие uploads на COPY и сравнить совместный scene run с single-DIRECT. Отправлять готовые пакеты без обязательного ожидания записи всей сцены. Реальное reuse каждого resource/allocator slot учитывает все очереди, которые его использовали.

## Test Scenarios

- Producer chain, diamond и два читателя на DIRECT/COMPUTE перед новой COPY-записью дают корректные данные.
- GPU wait завершён заранее / ещё не завершён: CPU не блокируется на ожидании вычисления, независимый DIRECT-префикс выполняется.
- Неверная циклическая зависимость отклоняется до submit; ошибка записи не оставляет отправленную очередь ждать token, который никто не подаст.
- Command lists из одной пачки не полагаются на несуществующий полный барьер между list boundaries; явные memory dependencies сохранены.
- Mixed DIRECT/COMPUTE emitters имеют те же состояния и общий прозрачный результат; несколько reader queues защищают повторную запись.
- Resize/shutdown после незавершённых packets дожидаются нужной работы, а steady-state не делает глобальный Flush каждый кадр.

## Acceptance and Handoff

CPU plan tests, GPU readbacks, image checks и debug layer/GBV на focused fixtures. GPU timeline показывает хотя бы один подходящий реальный участок перекрытия; если hardware сериализует workload, зафиксировать это и не считать одно наличие очередей доказательством ускорения. Отчёт содержит serial-vs-async timings и причину выбранных границ; ухудшение исследовать, а не скрывать изменением качества. Локальный коммит. Следующее задание: [U9](09-cross-adapter-transport.md).

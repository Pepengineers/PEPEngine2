---
title: "U3 Hierarchical parallel recording - Plan"
type: feat
date: 2026-11-13
---

# U3 Hierarchical parallel recording - Plan

## Contract

Выполнить только U3 из [мета-задания](00-meta-plan.md), R4–R8; KTD2–KTD7. Требуется принятый U2. По-прежнему одна карта и одна DIRECT-очередь: это параллельная CPU-запись, не GPU async.

## Outcome

Операция обновления объектов и минимум ещё одна независимая группа записывают command lists параллельно, включая дочерние jobs и continuation сборки. Порядок GPU-работы соответствует плану независимо от порядка завершения workers.

## Files

Новые `Apps/App.MetaRenderGraph/src/Graph/MetaGraphRecorder.*`, необходимые изменения compiler/executor U2. JobSystem расширяется в `Engine/Engine.Core/Include/Engine.Core/Jobs/JobSystem.h` и `Engine/Engine.Core/src/Jobs/JobSystem.cpp` только если текущий контракт не покрывает continuation. Сохранить существующие call sites.

Тесты: `Apps/App.MetaRenderGraph/tests/RecordingJobsTests.cpp`, дополнения `DirectExecutionGpuTests.cpp`; `Engine/Engine.Core/tests/JobSystemTests.cpp` при изменении job API. Отчёт `docs/reports/meta-render-graph/03-recording-jobs.md`.

## Approach

1. Freeze входы, destinations, диапазоны descriptors/uploads и allocator ownership до записи. Дочерняя job получает свой bucket и свой list; записывает результат в заранее назначенную ячейку массива.
2. Создать fan-out/fan-in без блокирующего ожидания дочерних jobs внутри worker. Не предполагать, что текущий ParallelJob::Schedule разрешён из worker: он owner-thread-only. Выбранная интеграция enkiTS/continuation должна сохранять время жизни объектов задач до завершения callback.
3. Пролог/эпилог группы записывают общие barriers один раз; bucket lists меняют только свои непересекающиеся диапазоны. Отдельные nested groups могут формировать несколько packets, а не один обязательный group-wide GPU fence.
4. CPU Ready публикуется после завершения записи и закрытия/проверки всех нужных lists. Queue submitter сериализует отправку, но не блокирует recording workers. Готовый ранний packet может быть отправлен до завершения поздней независимой recording group.
5. Оставить режим последовательной записи того же плана для сравнения. CPU buckets не заменяют GPU-driven ExecuteIndirect обычными CPU draw loops. Гранулярность выбирать по объёму записи; не создавать job/list на каждый мелкий объект по умолчанию.

## Test Scenarios

- 0/1/N buckets, разный порядок завершения и намеренно задержанный bucket дают тот же массив логических результатов и GPU readback, что serial recording.
- Parent→children→continuation работает при одном доступном worker и при обычном pool, без starvation/deadlock.
- Две top-level groups реально пересекаются на CPU trace; это не доказательство GPU async и так не маркируется.
- Несколько buckets пишут раздельные диапазоны одного destination: barriers только на границах группы, данные не перемешиваются.
- Job error/cancellation during shutdown не отправляет неполную пачку и сохраняет lifetime уже отправленных списков.
- Allocator, descriptor и upload ranges не переиспользуются, пока их прежний GPU-потребитель не завершён; повторные кадры проходят debug layer.

## Acceptance and Handoff

CPU и GPU tests проходят, serial/parallel результаты совпадают, CPU trace показывает параллельную запись. Отчёт фиксирует nesting/continuation API, владельцев ресурсов записи, поведение ошибки и доказательство раннего submission. Число workers и timings приводятся без обещания GPU/FPS-прироста. Локальный коммит. Следующее задание: [U4](04-geometry-shadows.md).

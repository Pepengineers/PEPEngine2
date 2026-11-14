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

Пути fixtures как ориентиры (создание и расширение не требуются): `Apps/App.MetaRenderGraph/tests/RecordingJobsTests.cpp`, дополнения `DirectExecutionGpuTests.cpp`; `Engine/Engine.Core/tests/JobSystemTests.cpp` при изменении job API. Отчёт `docs/reports/meta-render-graph/03-recording-jobs.md`.

## Approach

1. Freeze входы, destinations, диапазоны descriptors/uploads и allocator ownership до записи. Дочерняя job получает свой bucket и свой list; записывает результат в заранее назначенную ячейку массива.
2. Создать fan-out/fan-in без блокирующего ожидания дочерних jobs внутри worker. Не предполагать, что текущий ParallelJob::Schedule разрешён из worker: он owner-thread-only. Выбранная интеграция enkiTS/continuation должна сохранять время жизни объектов задач до завершения callback.
3. Пролог/эпилог группы записывают общие barriers один раз; bucket lists меняют только свои непересекающиеся диапазоны. Отдельные nested groups могут формировать несколько packets, а не один обязательный group-wide GPU fence.
4. CPU Ready публикуется после завершения записи и закрытия/проверки всех нужных lists. Queue submitter сериализует отправку, но не блокирует recording workers. Готовый ранний packet может быть отправлен до завершения поздней независимой recording group.
5. Оставить режим последовательной записи того же плана для сравнения. CPU buckets не заменяют GPU-driven ExecuteIndirect обычными CPU draw loops. Гранулярность выбирать по объёму записи; не создавать job/list на каждый мелкий объект по умолчанию.

## Smoke-check

Применяется [общий порядок проверки](00-meta-plan.md#verification-contract). Сцена работает с параллельной записью команд и выдаёт GPU-результат. Достаточно обычного короткого запуска без явных ошибок; уже полученный результат можно использовать повторно без нового прогона.

## Acceptance and Handoff

Зафиксировать API nesting/continuation и владельцев ресурсов записи. Кратко записать commit, выбранный режим и итог: работает / не работает / не запускалось, с причиной при наличии. Локальный коммит только своих изменений. Следующее задание: [U4](04-geometry-shadows.md); переходить только в разрешённых пользователем пределах.

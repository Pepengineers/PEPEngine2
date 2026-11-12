---
title: "U12 Integrated acceptance and delivery - Plan"
type: test
date: 2026-11-13
---

# U12 Integrated acceptance and delivery - Plan

## Contract

Выполнить только U12 из [мета-задания](00-meta-plan.md), проверить R1–R14 и Definition of Done. Требуется принятый U11 и отчёты всех предыдущих этапов. Этот этап не разрешает переключить основной App или удалить старый граф.

## Outcome

Воспроизводимо проверенное дополнительное приложение с новым execution plan, hierarchical CPU recording, same-GPU async и independent MGPU воды/частиц. Все необходимые исправления обнаруженных регрессий включены; итог честно отделяет подтверждённое от аппаратно недоступного.

## Files

`Apps/App.MetaRenderGraph/tests/SingleGpuSceneTests.cpp`, `CombinedMgpuSceneTests.cpp` и затронутые regression fixtures; исправления только по выявленным дефектам. `docs/reports/meta-render-graph/12-acceptance.md`, финальный `technique-inventory.md`, руководство запуска `Apps/App.MetaRenderGraph/README.md`.

## Approach

1. Сверить каждый R-ID, actual technique inventory и stage handoffs. Для каждой техники указать owner, packets/accesses, CPU recording mode, supported GPU placement и evidence. Не принять строку «переиспользуется старый граф» за миграцию.
2. Сопоставить режимы: legacy reference, new single-DIRECT serial recording, new single-DIRECT parallel recording, new same-GPU async, new MGPU water, new MGPU water+particles. Scene/camera/quality/particle counts фиксируются; real simulation steps записываются отдельно.
3. Проверить отсутствие hidden queue submissions, старого Execute, duplicate solver workers и обязательных CPU GPU-waits в steady state нового пути. Допустимые init/rebuild/shutdown waits назвать явно. Убедиться, что scopes, markers и timers охватывают отдельные export/import/history packets.
4. Снять CPU timeline записи и GPU timeline очередей/адаптеров. Показать хотя бы один actual nested recording group, late GPU wait, перекрытие вычисления/копирования и независимый secondary progress. Если тест hardware не даёт конкретного overlap, описать наблюдаемое и причину; наличие API queue не считать подтверждением.
5. Прогнать lifecycle regression и ограниченный повторяемый совместный run с несколькими оборотами всех slot rings. Исправить найденные проблемы в owning stage, сохранив интерфейсы или актуализировав handoff. Не превращать этап в бесконечную оптимизацию или автоподбор настроек.
6. Подготовить руководство: сборка/запуск, выбор экспериментального exe, adapters и placements, диагностический serial mode, расположение evidence и known limitations. Production App остаётся на прежнем executor.

## Test Scenarios

- Идентичный frame input даёт ожидаемое изображение/depth/motion во всех сопоставимых local режимах; solver comparison на MGPU идёт по одинаковым шагам.
- Startup, pause/resume, camera movement/cut, resize, reset/recreate, переключение допустимых экспериментальных placements и orderly shutdown не нарушают slots/history.
- Сочетание воды, многих эмиттеров, mixed local queues и UI не даёт частичной публикации, неверной сортировки или очереди, ожидающей никогда не сигнализируемый fence.
- Production App собирается и проходит релевантные старые проверки после shared seam изменений; его configs/default path не изменены.
- Повторный запуск по README на тех же adapters/settings воспроизводит результаты, а отсутствующее оборудование/SDK выдаёт честный unsupported/pending результат.

## Acceptance and Handoff

В итоговом отчёте: commits, matrix требований и техник, выполненные tests, adapters/driver/config, CPU recording/submission times, GPU queue/transfer/render times и FPS там, где сравнение корректно. Приложить ссылки на captures/logs с manifest, не включать тяжёлые приватные/машинные artifacts в commit без необходимости.

Не заявлять 100% загрузку как обязательное свойство графа. Не принимать непроверенный MGPU как завершённый только из-за успешной сборки. Если остались hardware gates, завершённый код и pending проверки перечисляются отдельно; meta Definition of Done ещё не достигнут.

Создать локальный коммит исправлений/документации только своей задачи. Отдельным последующим решением пользователя могут быть интеграция нового executor в основной App и удаление старого; в этом пакете они не выполняются.

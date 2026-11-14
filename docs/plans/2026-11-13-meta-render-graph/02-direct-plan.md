---
title: "U2 Single DIRECT execution plan - Plan"
type: feat
date: 2026-11-13
---

# U2 Single DIRECT execution plan - Plan

## Contract

Выполнить только U2 из [мета-задания](00-meta-plan.md), R1–R3, R5–R8; KTD1–KTD5, KTD7. Требуется принятый U1 и его отчёт. Все команды нового пути идут на одной DIRECT-очереди одного устройства. CPU-запись пока последовательная.

## Outcome

Экспериментальный App действительно исполняет новый план с явными ресурсами и пакетами. Простая сцена и GPU buffer fixtures работают без вызова GDX12RenderGraph::Execute внутри нового пути.

## Files

Создать `Apps/App.MetaRenderGraph/src/Graph/MetaGraph.*`, `MetaGraphCompiler.*`, `MetaGraphExecutor.*`, `src/MetaRenderPipeline.*`. Имена допускают уточнение, ответственность — нет. Минимально расширить `Apps/App.Base/src/RenderModule.cpp` и его owning headers для opt-in seam из U1. Existing queue/list/resource wrappers менять только там, где необходим новый record-context при сохранении старого пути.

Для smoke при необходимости использовать существующие fixtures; новые test targets не обязательны. Отчёт `docs/reports/meta-render-graph/02-direct-plan.md`. Ориентиры: старые `GDX12RenderGraph.*`, `GDX12Resource.*`, `GDX12CommandList.*`, `GDX12CommandQueue.*`, `Apps/App/tests/RenderGraphTests.cpp`.

## Approach

1. Ввести операции с accesses, explicit dependencies и побочными эффектами. Компилятор определяет порядок, boundary states, barriers и packets. Completion tokens выражают зависимости; backend пока единственный DIRECT. Не реализовывать неиспользуемые multi-device режимы заранее.
2. Для ресурсов использовать заранее созданные handles/slots и явные ёмкости; без нового aliasing allocator. Учитывать запись после всех читателей и UAV hazards при неизменном состоянии. Отсутствие transition не означает отсутствие зависимости.
3. Отделить локальные состояния записи от mutable глобального GDX12Resource tracker. Обеспечить путь переходов, пригодный будущим параллельным jobs: заранее известные before/after и explicit internal accesses. Проверить hidden barriers внутри вызванных helpers. Не решать порядок CPU mutex вокруг общего tracker.
4. Новый executor управляет Close/Execute/Signal и retirement command allocators/descriptors/upload slots. Счётчики queue fences принадлежат одному владельцу. Частично записанный/ошибочный пакет не отправляется и не публикует успешный token.
5. Добавить CPU-only представление schedule для тестирования и диагностики: packet ID, accesses, barriers, зависимости и queue. Не создавать графический редактор или общий framework сериализации графов.
6. Подключить procedural fixture: upload → compute transform на DIRECT → draw/readback → Present. Работа synthetic fixture должна идти через тот же executor, который используется приложением. Допустим минимальный вывод без пока не подключённых техник.

## Smoke-check

Применяется [общий порядок проверки](00-meta-plan.md#verification-contract). Новый executor выполняет GPU-работу на DIRECT и выдаёт результат. Достаточно обычного короткого запуска без явных ошибок; уже полученный результат можно использовать повторно без нового прогона.

## Acceptance and Handoff

Зафиксировать API плана/executor, правила состояний и retirement для U3. Кратко записать commit, выбранный режим и итог: работает / не работает / не запускалось, с причиной при наличии. Локальный коммит только своих изменений. Следующее задание: [U3](03-recording-jobs.md); переходить только в разрешённых пользователем пределах.

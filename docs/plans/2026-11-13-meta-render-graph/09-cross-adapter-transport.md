---
title: "U9 Cross-adapter transport and independent loops - Plan"
type: feat
date: 2026-11-13
---

# U9 Cross-adapter transport and independent loops - Plan

## Contract

Выполнить только U9 из [мета-задания](00-meta-plan.md), R5–R8, R10–R11, R14; KTD7–KTD10. Требуется принятый U8. Здесь проверяется MGPU-механизм на детерминированном buffer producer, без миграции воды/частиц.

## Outcome

Один executor управляет packet-зависимостями на двух физических адаптерах. Вторичный COMPUTE producer, вторичная COPY, основная COPY и основной рендер работают независимо в пределах защищённых слотов. Нет привязки secondary tick к номеру Present.

## Files

Расширить Graph queue/device identity; добавить `Apps/App.MetaRenderGraph/src/Graph/CrossAdapterTransfer.*`, `PublicationSlots.*`, `ProducerLoop.*` только по фактической потребности. Не строить новый универсальный placement/quality runtime.

Тесты: `Apps/App.MetaRenderGraph/tests/CrossAdapterPlanTests.cpp`, `CrossAdapterTransportGpuTests.cpp`; небольшой fixture shader в собственном каталоге. Ориентир low-level cross-adapter handles/fences: `Apps/App/src/Rendering/Water/GDX12WaterOffload.cpp`, существующие WaterOffloadGpuTests. Отчёт `docs/reports/meta-render-graph/09-cross-adapter-transport.md`.

## Approach

1. Выбирать две разные аппаратные карты явно, не использовать третью. Resource identity включает device: одинаковое имя/offset на разных devices не означает один локальный ресурс. Shared heap имеет отдельные local handles и общую публикацию.
2. Выразить цепочку secondary COMPUTE produce/pack → secondary COPY export → shared ready → primary COPY import → primary DIRECT consumer через те же packets/tokens, что U8. Не добавлять transport, самовольно отправляющий queues вне executor.
3. Создать заранее выделенные packed/shared/display slots. Три независимых lifetime: packed до export complete, shared до import complete, display до последнего render consumer. Metadata/size/sequence/data принадлежат одной публикации. Bounds и generation проверяются перед использованием.
4. Реализовать выбор StrictPublication и LatestCompleted согласно KTD10. LatestCompleted выбирает готовые данные и сохраняет прежний display, если новый импорт ещё не завершён; не ставит весь основной DIRECT за будущим solver. Strict связывает реального producer и consumer без CPU GPU-wait.
5. Independent loops создают свои inputs/recording jobs/submissions и не требуют общей frame barrier. В каждом цикле используются существующие slots; отсутствие свободного slot не разрешает overwrite или unbounded allocation. Producer может продолжить расчёт в собственных безопасных resources без обязательной публикации результата.
6. Выполнить реальные CPU recording jobs для обеих карт параллельно, с отдельными contexts и общим механизмом очередей. Развести ожидание свободного ресурса/continuation и блокирование worker pool. Зафиксировать поведение producer failure и orderly stop, чтобы primary не остался навсегда ждать невыданный token.

## Test Scenarios

- Производитель заполняет payload известной последовательностью, primary readback побитно совпадает для выбранного sequence через несколько оборотов всех слотов.
- Primary быстрее secondary и наоборот: последний готовый результат корректен, нет частичной копии, overwrite или зависимости на frame index другой карты.
- Primary Present временно не вызывается, secondary producer продолжает шаги; отсутствие потребления не портит опубликованные данные.
- Strict и LatestCompleted различаются выбором версии, а не отключением fences. Заранее готовый результат не вызывает ненужный CPU wait.
- Two-reader display resource возвращается в reuse только после обоих readers; cross-iteration зависимости не создают взаимного ожидания.
- Нет второй карты / выбрана та же карта: явная диагностика конфигурации, без выдачи single-GPU fallback за успешный MGPU-тест.
- Shutdown с заполненными slots и GPU in-flight завершается без use-after-free; packet error не публикует фиктивный successful result.

## Acceptance and Handoff

Две реальные карты, exact payload checks, debug layer и timeline с реальным COMPUTE/COPY/DIRECT исполнением. Записать adapters/LUIDs, bytes, publication sequences, waits/signals и source paths. Без такого оборудования результат — hardware validation pending, U9 не принят. Математика production solver здесь ещё не проверяется. Локальный коммит. Следующее задание: [U10](10-mgpu-water.md).

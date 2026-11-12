---
title: "U5 Lighting and reflections - Plan"
type: refactor
date: 2026-11-13
---

# U5 Lighting and reflections - Plan

## Contract

Выполнить только U5 из [мета-задания](00-meta-plan.md), R2–R3, R5–R9; KTD2–KTD5. Требуется принятый U4. Одна карта, DIRECT-only GPU execution, параллельная CPU-запись независимых lists сохраняется.

## Outcome

Environment/IBL, optional clouds, reflection probes, AO, deferred lighting и SSR подключены к новому executor с исходными алгоритмами. Промежуточная сцена имеет корректное освещение без воды/частиц и окончательной post chain.

## Files

Adapters в `Apps/App.MetaRenderGraph/src/Techniques/` и MetaRenderPipeline. Источники: `Apps/App.Base/Rendering/CommonRenderPasses` по фактическим Include/src путям, `Apps/App/src/Rendering/BistroRenderPipeline.cpp`, renderer passes EnvironmentIBL/ReflectionProbe/AmbientOcclusion/DeferredLighting/SSSR и cloud source.

Тесты: `Apps/App.MetaRenderGraph/tests/LightingGpuTests.cpp`, `ReflectionGpuTests.cpp`. Ориентиры существующих checks: `Apps/App/LightingGpuTests.vcxproj`, `AOTests.vcxproj`, `ReflectionProbeTests.vcxproj`, `SSRBistroTests.vcxproj`. Отчёт `docs/reports/meta-render-graph/05-lighting-reflections.md`.

## Approach

1. U5.1: environment/IBL и optional local clouds с реальными prerequisites, history и time scale. Не вернуть удалённый MGPU cloud wrapper.
2. U5.2: probe capture/filter/publication с их geometry views и текущим расписанием обновления. Probe result публикуется после необходимых этапов, а не после одной CPU recording job; cache semantics сохраняются.
3. U5.3: AO в поддерживаемых early/late вариантах и deferred lighting. Реальные depth/normal/lighting reads/writes входят в план. Один API packet не обязан соответствовать одному shader dispatch.
4. U5.4: SSR и composition с предыдущими результатами. Отделить persistent history от frame-local resources. Не делить проходы исключительно ради количества jobs; параллелить независимую запись по доступным inputs/state contracts.
5. Для каждого optional pass описать disabled/fallback выход, чтобы потребители не читали отсутствующую либо устаревшую texture. Обновить inventory и baseline после подгруппы.

## Test Scenarios

- Serial/parallel recording одного плана дают одинаковые outputs при фиксированных scene/camera и истории.
- Early/late AO: корректный порядок input depth и отсутствие сломанного lighting после переключения.
- Probe впервые готов, повторно используется из cache и обновляется после движения сцены согласно текущим правилам; старые данные не публикуются как новые.
- Clouds выключены/включены, мир paused/time-scaled: source и история соответствуют текущему поведению.
- SSR off/on, camera cut/reset и resize не оставляют неинициализированные history reads.
- Ни одна техника не вызывает скрытый queue submit/CPU wait из recording worker; debug layer чист на fixture.

## Acceptance and Handoff

Пройденные новые fixtures, сравнение с baseline при тех же настройках, explicit packets/accesses для всех U5-техник. Существующие GPU тесты без подключения нового executor не засчитывать как единственное доказательство. Локальный коммит и отчёт с реальными dependencies для воды/прозрачности. Следующее задание: [U6](06-water-particles.md).

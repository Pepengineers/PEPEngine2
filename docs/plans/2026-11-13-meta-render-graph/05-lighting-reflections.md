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

Пути fixtures как ориентиры (создание и расширение не требуются): `Apps/App.MetaRenderGraph/tests/LightingGpuTests.cpp`, `ReflectionGpuTests.cpp`. Ориентиры существующих checks: `Apps/App/LightingGpuTests.vcxproj`, `AOTests.vcxproj`, `ReflectionProbeTests.vcxproj`, `SSRBistroTests.vcxproj`. Отчёт `docs/reports/meta-render-graph/05-lighting-reflections.md`.

## Approach

1. U5.1: environment/IBL и optional local clouds с реальными prerequisites, history и time scale. Не вернуть удалённый MGPU cloud wrapper.
2. U5.2: probe capture/filter/publication с их geometry views и текущим расписанием обновления. Probe result публикуется после необходимых этапов, а не после одной CPU recording job; cache semantics сохраняются.
3. U5.3: AO в поддерживаемых early/late вариантах и deferred lighting. Реальные depth/normal/lighting reads/writes входят в план. Один API packet не обязан соответствовать одному shader dispatch.
4. U5.4: SSR и composition с предыдущими результатами. Отделить persistent history от frame-local resources. Не делить проходы исключительно ради количества jobs; параллелить независимую запись по доступным inputs/state contracts.
5. Для каждого optional pass описать disabled/fallback выход, чтобы потребители не читали отсутствующую либо устаревшую texture. Обновить inventory после подключения подгрупп.

## Smoke-check

Применяется [общий порядок проверки](00-meta-plan.md#verification-contract). Новый путь отображает освещение и отражения. Достаточно обычного короткого запуска без явных ошибок; уже полученный результат можно использовать повторно без нового прогона.

## Acceptance and Handoff

Указать зависимости освещения и отражений для воды/прозрачности. Кратко записать commit, выбранный режим и итог: работает / не работает / не запускалось, с причиной при наличии. Локальный коммит только своих изменений. Следующее задание: [U6](06-water-particles.md); переходить только в разрешённых пользователем пределах.

---
title: "U4 Geometry and shadows - Plan"
type: refactor
date: 2026-11-13
---

# U4 Geometry and shadows - Plan

## Contract

Выполнить только U4 из [мета-задания](00-meta-plan.md), R1–R9; KTD1–KTD7. Требуется принятый U3. Новый путь использует одну DIRECT-очередь и параллельную CPU-запись. Инвентарь U1 определяет актуальные техники; не менять порядок ECS-систем ради render jobs.

## Outcome

Новый executor рисует геометрию реальной сцены и её тени, включая динамические данные объектов и траву. Подключение идёт небольшими законченными подгруппами; не требуются lighting/post техники следующего этапа, допустим диагностический вывод GBuffer/depth.

## Files

Новые adapters в `Apps/App.MetaRenderGraph/src/Techniques/`, изменения MetaRenderPipeline и минимальные общие recording seams. Ориентиры: `Apps/App.Base/src/RenderModule.cpp`, `Engine/Engine.RendererDX12/src/GDX12RenderGraph.cpp`, `Engine/Engine.RendererDX12/Include/Engine.RendererDX12/RenderPasses/`, `Apps/App/src/Rendering/BistroRenderPipeline.cpp`, `BistroGrassGeometry.*`.

Пути fixtures как ориентиры (создание и расширение не требуются): `Apps/App.MetaRenderGraph/tests/GeometryGpuTests.cpp`, `ShadowGpuTests.cpp`; existing `Apps/App/tests/ObjectDataJobsTests.cpp`, `RenderPipelineTests.cpp`, `BistroRenderGraphTests.cpp` служат примерами проверок, но не доказательством нового пути. Отчёт `docs/reports/meta-render-graph/04-geometry-shadows.md`.

## Approach

1. U4.1: подключить immutable scene snapshot, object/material/frame uploads, GPU culling, static/instanced geometry и depth/GBuffer. Обновления объектов распределить по buckets по модели U3. Сохранить indirect аргументы и GPU-driven draw-путь.
2. U4.2: внести skinning и прочие подготовительные GPU-команды, которые текущий RenderModule пишет вне графа, в явные операции. Рабочая геометрия обновляется при движении/анимации; фиксированный состав сцены не означает статические данные.
3. U4.3: подключить cascaded sun/local shadows и необходимые geometry views. Параллельно записывать независимые views только с раздельными constants/descriptors/state contexts. Проверить actual helpers на внутренние изменения shared state.
4. U4.4: подключить grass preparation, wind/interaction и per-view expansion для depth/GBuffer/shadow/probe views, которые действительно используют траву. Пока всё исполняется на DIRECT; сохранить reset/pause/transform и текущие настройки качества.
5. После подключения подгрупп обновить инвентарь. Техника, требующая ещё не подключённого входа, получает явный диагностический режим только для промежуточной проверки; итог U4 не скрывает отсутствующие входы.

## Smoke-check

Применяется [общий порядок проверки](00-meta-plan.md#verification-contract). Новый путь отображает геометрию и тени. Достаточно обычного короткого запуска без явных ошибок; уже полученный результат можно использовать повторно без нового прогона.

## Acceptance and Handoff

Обновить инвентарь подключённых geometry/shadow packets. Кратко записать commit, выбранный режим и итог: работает / не работает / не запускалось, с причиной при наличии. Локальный коммит только своих изменений. Следующее задание: [U5](05-lighting-reflections.md); переходить только в разрешённых пользователем пределах.

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

Тесты: `Apps/App.MetaRenderGraph/tests/GeometryGpuTests.cpp`, `ShadowGpuTests.cpp`; existing `Apps/App/tests/ObjectDataJobsTests.cpp`, `RenderPipelineTests.cpp`, `BistroRenderGraphTests.cpp` служат примерами проверок, но не доказательством нового пути. Отчёт `docs/reports/meta-render-graph/04-geometry-shadows.md`.

## Approach

1. U4.1: подключить immutable scene snapshot, object/material/frame uploads, GPU culling, static/instanced geometry и depth/GBuffer. Обновления объектов распределить по buckets по модели U3. Сохранить indirect аргументы и GPU-driven draw-путь.
2. U4.2: внести skinning и прочие подготовительные GPU-команды, которые текущий RenderModule пишет вне графа, в явные операции. Рабочая геометрия обновляется при движении/анимации; фиксированный состав сцены не означает статические данные.
3. U4.3: подключить cascaded sun/local shadows и необходимые geometry views. Параллельно записывать независимые views только с раздельными constants/descriptors/state contexts. Проверить actual helpers на внутренние изменения shared state.
4. U4.4: подключить grass preparation, wind/interaction и per-view expansion для depth/GBuffer/shadow/probe views, которые действительно используют траву. Пока всё исполняется на DIRECT; сохранить reset/pause/transform и текущие настройки качества.
5. После каждой подгруппы включить соответствующий fixture, записать baseline и обновить инвентарь. Техника, требующая ещё не подключённого входа, получает явный диагностический режим только для промежуточной проверки; итог U4 не скрывает отсутствующие входы.

## Test Scenarios

- Те же scene/camera/seed при serial и parallel recording дают совпадающие object/indirect buffers и изображения с заранее выбранной метрикой.
- Движение объекта и изменение трансформации обновляют depth, GBuffer и тени; animated fixture обновляет skinned geometry.
- Несколько shadow views записываются параллельно без смешивания camera constants и descriptor ranges.
- Пустой bucket, невидимые объекты и отсутствие grass patches не оставляют старые draw counts.
- Трава корректно участвует в положенных geometry views; interaction field имеет заявленную версию и не берётся из незавершённого кадра.
- Повторное использование нескольких frame slots, resize и shutdown проходят debug layer; старый App сохраняет поведение.

## Acceptance and Handoff

Все U4-позиции inventory имеют новый packet/access path и проверки. CPU trace показывает параллельную запись meaningful geometry/upload work; diagnostic screenshot/readbacks доказывают GPU-результат. Отчёт указывает временно последовательные внутренние части и их причину, не скрывая их за словом parallel. Локальные коммиты по подгруппам допустимы. Следующее задание: [U5](05-lighting-reflections.md).

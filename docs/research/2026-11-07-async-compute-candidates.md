# Кандидаты на async compute после частиц

Дата: 2026-11-07. Базовый HEAD при анализе: `dd7640b`. Во время анализа появились параллельные незакоммиченные изменения воды; они не принадлежат этой задаче. Указатели на строки относятся к прочитанному рабочему дереву и могут сдвинуться. Архитектурный рейтинг не является результатом A/B async-бенчмарка или утверждённым планом реализации.

## Цель и границы

Пользователь выбрал снижение GPU-времени полного кадра без изменения картинки и без дополнительного кадра задержки данных. Сам перенос dispatch на COMPUTE не является успехом. Сохраняются ранее согласованные default-off переключатели отдельных техник и DIRECT fallback ([предыдущий план](../tasks/async-compute-rollout.md)).

Здесь «перенос техники» означает перенос конкретной вычислительной стадии. Например, у воды переносится SPH solver, а растеризация поверхности остаётся на DIRECT. Это уточнение анализа, а не новый термин предметной области для `CONTEXT.md`.

## Вывод

Первый кандидат — **симуляция воды**: она уже вычислительная, не требует depth текущего кадра и может завершиться перед отрисовкой воды того же кадра. Изолированный замер новой нагрузки дал 12.7–12.8 ms; методика ниже. Второй кандидат — **атмосфера/облака и обновление environment IBL**, если они действительно выполняются в выбранной сцене. AO перспективен условно: текущая конфигурация использует поздний AO, а переключение на ранний меняет источник нормалей.

Рейтинг ниже оценивает зависимости и стоимость переделки. Он не утверждает порядок по миллисекундам: измерен только solver воды; сравнительные стоимости остальных кандидатов, конкуренция за GPU и итоговый выигрыш async не установлены.

| Стадия | Возможное окно в текущем кадре | Что мешает простому переносу |
| --- | --- | --- |
| **SPH-симуляция воды** | После подготовки snapshot/констант → до первого draw поверхности воды; потенциальное перекрытие depth, shadows, reflection capture, opaque | Сейчас solver и raster находятся в одном проходе; нужно выделить solver и объявить его буферы/потребителей |
| **Атмосфера, облака, environment IBL** | Начало кадра → первый потребитель environment в reflections/lighting; потенциально depth и shadows | Выходы — persistent UAV-текстуры; граф пока не поддерживает async texture writes. LUT/IBL имеют разные условия обновления |
| **AO с сохранением выбранного execution point** | Поздний AO: после opaque normals → до lighting, потенциально параллельно surface воды. Уже выбранный ранний AO: после prepass → до lighting либо более раннего ресурсного конфликта | UAV-текстуры и SDK states; у раннего/позднего режима разные normal inputs. Широкое окно раннего режима нельзя приписывать текущему позднему режиму |
| **Фильтрация reflection probes** | После capture шести граней → до потребителя опубликованной probe; потенциально opaque | Выделение из смешанного raster/compute прохода, scratch и publication lifetime; обновления выполняются не каждый кадр |
| **Wind field и загрузка корней травы** | До первого потребителя травы; возможно перекрытие независимой обычной геометрии | Текущая общая цепочка geometry заставляет весь depth ждать подготовки; требуется более точная зависимость |
| **GPU skinning** | До первого draw skinned mesh | Сейчас UAV-запись в общий vertex buffer; нельзя одновременно читать его статическую геометрию на DIRECT. Нужны отдельные выходы или узкое независимое окно |
| **GPU culling** | До первого чтения indirect draws/Visibility | Потребитель — уже ранний depth; без изменения расписания окно мало |
| **Grass expansion** | До draw конкретного view/patch | Один переиспользуемый vertex/index/indirect arena, результат сразу потребляется; нужны дополнительные буферы и учёт VRAM/lifetime |
| **Deferred lighting, SSR, DoF, motion blur, reconstruction** | Ограничено цепочкой зависимостей финального цвета | Перенос всего прохода обычно лишь перемещает последовательную работу. Выделение независимой подготовки — отдельная задача, не готовый выигрыш |

## Основания в коде

- Вода: [GDX12FluidPass.cpp](../../Apps/App/src/Rendering/Water/GDX12FluidPass.cpp), `Simulate` (289), `DrawSurface` (389), их последовательные вызовы (535, 551). Solver использует собственное состояние/константы; scene depth/motion нужны построению поверхности. В исходном HEAD один бассейн с 1024 частицами; параллельная правка [BistroWater.h](../../Apps/App/src/BistroWater.h) меняет seed на объёмную сетку около 50000 частиц и параметры давления. Для измерения зафиксирована отдельная копия новой нагрузки; старые и новые значения нельзя смешивать. Fluid включён, качество Mid: [GDX12Fluid.cpp](../../Apps/App/src/Rendering/Water/GDX12Fluid.cpp), 9–12.
- Environment: [GDX12EnvironmentIBLPass.cpp](../../Engine/Engine.RendererDX12/src/GDX12EnvironmentIBLPass.cpp), `Execute` (237), вызов clouds (246), cached return (325), условная IBL-фильтрация (344–355), покадровые sky-view/aerial LUT при atmospheric source (364–376). Неизменяемый skybox не даёт такой постоянной нагрузки. [GDX12CloudComponent.cpp](../../Engine/Engine.RendererDX12/src/GDX12CloudComponent.cpp), compute dispatch (143).
- AO: [Config/Engine.ini](../../Config/Engine.ini), 12: `Render.AO.ExecutionPoint = 1`; [GDX12AmbientOcclusion.cpp](../../Engine/Engine.RendererDX12/src/GDX12AmbientOcclusion.cpp), 25: `BeforeLighting`. [CommonRenderPasses.cpp](../../Apps/App.Base/src/Rendering/CommonRenderPasses.cpp), 42–45 выбирает `Depth` или `Opaque` normals. [GDX12AmbientOcclusionPass.cpp](../../Engine/Engine.RendererDX12/src/GDX12AmbientOcclusionPass.cpp) содержит compute пути SSAO/HBAO/XeGTAO/CACAO. Эквивалентность картинки раннего и позднего AO не установлена.
- Probes: [GDX12ReflectionProbePass.cpp](../../Engine/Engine.RendererDX12/src/GDX12ReflectionProbePass.cpp), compute-фильтрация 540–565, вызов после capture 814. Требуется сохранять момент публикации текущего результата.
- Трава: [BistroRenderPipeline.cpp](../../Apps/App/src/Rendering/BistroRenderPipeline.cpp), отдельная подготовка 10–17, expand/draw 32–45, общая geometry-цепочка 172–178. [GDX12GrassRenderer.cpp](../../Apps/App/src/Rendering/Grass/GDX12GrassRenderer.cpp), wind 329–359, expansion 572–601, защита от раннего reuse arena 616.
- Skinning: [RenderModule.cpp](../../Apps/App.Base/src/RenderModule.cpp), dispatch перед графом 944; [GDX12Skinning.cpp](../../Engine/Engine.RendererDX12/src/GDX12Skinning.cpp), 142–166; [GDX12GeometryBuffer.cpp](../../Engine/Engine.RendererDX12/src/GDX12GeometryBuffer.cpp), 125–143 переводит весь vertex resource в UAV и обратно.
- Culling: [GDX12GPUCullingPass.h](../../Engine/Engine.RendererDX12/Include/Engine.RendererDX12/RenderPasses/GDX12GPUCullingPass.h), 7–12 и 34–73; Visibility нужен ранним geometry passes.
- Хвост кадра: [BistroRenderPipeline.cpp](../../Apps/App/src/Rendering/BistroRenderPipeline.cpp), 187–202: lighting → reflection preview → SSR → water/transparency/composition → DoF → motion blur → reconstruction. Явные зависимости transparency от SSR и water resolve находятся на 192 и 196. Поэтому SSR не получает свободного перекрытия с текущей transparency простым переключением очереди.

## Общие ограничения инфраструктуры

В [GDX12RenderGraph.cpp](../../Engine/Engine.RendererDX12/src/GDX12RenderGraph.cpp):

1. Async texture accesses должны быть явно объявленным чтением; запись отклоняется на 448–449. AO/environment/probe outputs требуют поддержки передачи UAV-текстур между очередями, состояний и lifetime. Нельзя скрывать выходы от графа ради обхода проверки.
2. Единственная pending async-ветка закрывается перед следующим async node (405–417). Например, ранняя вода на COMPUTE может быть принудительно завершена перед particle simulation после depth, значительно сокращая предполагаемое окно. Для совместного режима надо либо доказать достаточность этого окна измерением, либо расширить scheduler с корректными зависимостями и retirement.
3. Существующая buffer-передача и финальный join могут служить основой для solver воды (457–478), но все физические буферы и доступы должны быть объявлены.
4. Выбор compute queue сейчас связан только с particle setting: [CommonRenderPasses.cpp](../../Apps/App.Base/src/Rendering/CommonRenderPasses.cpp), 50. [RenderModule.cpp](../../Apps/App.Base/src/RenderModule.cpp), 951–952 отключает этот путь при dual-GPU. Новая техника должна работать независимо от particle toggle и сохранять DIRECT fallback.
5. Текущие агрегированные async statistics хранят последние fork/compute/join и один compute timer. При нескольких стадиях их недостаточно для подробной атрибуции; полный GPU frame interval и GPU Trace остаются необходимыми.

Это ограничения текущего движка, а не запрет D3D12 на вычислительные выходные текстуры. Требования к межочередной синхронизации и владению ресурсами описаны в [Microsoft: Multi-engine synchronization](https://learn.microsoft.com/en-us/windows/win32/direct3d12/user-mode-heap-synchronization).

## Если первым выбирать воду

Минимальная отдельная итерация: выделить solver из `Surface`, сохранить DIRECT baseline после разделения, затем добавить opt-in COMPUTE и join перед первым потребителем состояния воды. Отрисовка/фильтрация поверхности и optics сохраняют свою позицию.

Обязательно сохранить:

- Один шаг на активный instance с тем же snapshot `DeltaSeconds`, pause/time scale, safe timestep и bounded catch-up.
- Seed upload и `SavePrevious` до раннего возврата при остановленной симуляции/нулевом dt (`Simulate`, 291–325).
- Независимость `RenderEnabled`/`SimulationEnabled`, history/reset bookkeeping и порядок записи констант (453–477, 504–554). Перенос CPU-side history flags раньше может затронуть другие проходы; нельзя считать его автоматически эквивалентным.
- Завершение compute до повторного использования frame slot, освобождения seed upload/state, изменения controls и resize. DIRECT fence достаточен только если он транзитивно покрывает нужный compute join.
- Корректный режим вместе с particles async: одиночный тест воды не проверяет взаимодействие двух async-кандидатов.

Точки изменений: water pass/состояние, Bistro pipeline и bindings, декларации buffer accesses, при необходимости executor, controls; существующие `FluidSimulationChecks`, `BistroRenderGraphTests`, `RenderGraphTests` и проверки pipeline. Шейдерная математика solver не требует изменения ради смены очереди.

## Как выбрать по измерениям

1. Зафиксировать сцену/камеру/разрешение/quality и измерить стоимость solver воды и активных environment-стадий, их первых потребителей и свободное окно. Для воды нужен воспроизводимый ненулевой dt: нулевой dt пропускает сам solver и измеряет другую нагрузку.
2. Отдельно проверить DIRECT после разделения против исходного baseline, затем DIRECT/async/DIRECT с одинаковыми входами и частицами off/on. Сравнивать полный GPU frame median/p95, CPU submit и GPU timeline, а не сумму длительностей очередей.
3. Проверить GBV, parity состояния симуляции и изображения, pause/hidden rendering/reset/resize/toggles и queued frames. Переиспользовать существующие fluid/graph/Bistro проверки, добавив проверки новых границ владения.
4. Оставлять оптимизацию по подтверждённому сокращению полного кадра. Overlap может конкурировать за память/вычислители и замедлять кадр; это причина измерять сочетания, а не включать всё разом. [NVIDIA: Async Compute and Overlap](https://developer.nvidia.com/blog/advanced-api-performance-async-compute-and-overlap/) рекомендует выбирать пары по загрузке GPU-блоков и проверять итоговое время полного кадра.

## Первый замер воды

После выбора пользователя измерен solver на **RTX 3060 Laptop, 6144 MiB, driver 617.42**. В отдельную диагностическую сборку скопированы текущие water C++/HLSL и seed из параллельной правки: **51975 частиц**, один бассейн, `dt = 1/60`, rendering выключен, simulation включена. Это новая нагрузка, не прежняя сцена с 1024 частицами.

Три серии с одинаковым seed/reset: 24 прогревочных + 120 измеренных шагов в каждой. GPU timestamps охватывают `water.Execute`: solver, `SavePrevious` и его barriers; surface raster/filter/optics не выполняются. После каждого шага выполняется CPU drain. Частоты GPU не менялись. Validation отключена во время измерения.

| Серия | Median, ms | p95, ms | Stdev, ms | Min–max, ms |
| --- | ---: | ---: | ---: | ---: |
| 0 | 12.760 | 14.126 | 1.037 | 10.836–15.301 |
| 1 | 12.849 | 14.091 | 1.002 | 10.947–15.273 |
| 2 | 12.686 | 14.089 | 1.054 | 10.734–15.351 |

Вода представляет заметную вычислительную нагрузку и заслуживает первой проверки overlap. **12.7–12.8 ms — стоимость текущей работы, а не ожидаемая экономия async.** Объём скрываемой работы ограничен независимым участком DIRECT и конкуренцией за GPU. Полный кадр Bistro, его свободное окно и реальный queued throughput здесь не измерены.

Проверки: диагностическая Release/x64 сборка прошла; отдельный короткий GBV-прогон (1 warmup + 2 шага) прошёл без warnings/errors. Fixture использует существующий `GDX12OpaquePass` для валидных depth/motion inputs. Локальный LNK1103 устранён `/Z7 /GL-` только у диагностических translation units; используются готовые библиотеки движка. Production-файлы этой задачей не менялись. Проверки численной корректности solver и визуальной идентичности этим прогоном не заменяются.

Артефакты:

- [360 исходных samples](2026-11-07-water-solver-timing.csv) и [методика, статистика, SHA256 исходников/EXE](2026-11-07-water-solver-timing.json).
- Локальная замороженная копия и harness: `Build/AsyncWater-20261107/`; EXE: `Build/bin/x64/Release/WaterTiming/WaterTiming.exe`.
- Успешные runner receipts: `Build/AsyncWater-20261107/build3.json`, `validation2.json`, `timing.json`; полные stdout/stderr рядом. EXE, solver HLSL, параметры HLSL и runtime INI сохранили fingerprints до/после timing run.

Следующее инженерное доказательство перед переносом — стоимость и длительность независимых DIRECT-стадий на полном кадре той же замороженной нагрузки. Одного solver-only замера недостаточно для решения включать async.

## Статус решений и проверки этого отчёта

- Согласована цель: меньше GPU-время, та же картинка, без дополнительной задержки данных.
- Пользователь выбрал следующим шагом измерение воды. Перенос на COMPUTE пока не выбран и не начат.
- ADR не создан: необратимого архитектурного выбора пока нет. `CONTEXT.md` не изменён: общие GPU-термины не относятся к специфическому словарю проекта.
- Проверены исходники, конфигурация, ссылки отчёта, samples и diff; выполнены диагностическая сборка, короткий GBV и три серии solver timing. Полный Bistro, GPU Trace, async A/B и другие GPU не проверены. Доказательства ускорения или визуальной идентичности переноса нет.

# Bistro: фактическая occupancy рендера

Дата анализа: 2026-11-03. Runtime и настройки сцены не изменялись.

Средняя заполненность warp slots — **27,54%**, p95 по времени PM-сэмплов —
**72,64%**. Это невысокая средняя occupancy; дорогие геометрические проходы
имеют гораздо меньшую заполненность, чем fullscreen compute/post-processing.
Повышение occupancy возможно, но ускорение кадра нужно проверять A/B:
заполненные warp slots могут содержать ожидающие данные warps.

## Измерение и границы

Повторно прочитан существующий захват оптимизированного CPU-prepared варианта:
`Build/Captures/2026-11-02-render-preparation/prepared-1.ngfx-gputrace`.
Это измерение сборки `73d8066`, EXE SHA-256
`449BC8D9498B8CAA0673E297686DC8BDCB5F82A4B9D66182ED2987530FB6C8AD`;
точный SHA захвата —
`A257771BF6A242C3644FC0ED40423F3EBEF678132F8718DBBB276B5EA24C9837`.

Условия из [манифеста предыдущего исследования](2026-11-02-render-preparation-shadow-efficiency-results.md):
RTX 3060 Laptop, драйвер 610.88, Release x64 с profiling scopes, 1280×800,
XeSS, defaults сцены, неподвижная камера у входа, продолжающаяся симуляция.
Частота GPC в повторно прочитанных PM-сэмплах — около 900 MHz.
Окно: 12 кадров, 405,329088 мс, среднее 33,777424 мс/кадр.
Это профилируемый сценарий, не обычный FPS benchmark.

Текущий HEAD при анализе — `b9a9772`; после записанного trace добавлен
tone-map state fix `7e10358`. Свежая occupancy именно этого HEAD не снималась.
Существующий `Build/bin/x64/Release/App/App.exe` от 2026-11-02 20:02
старее исследованного варианта и для выводов не использован.

Использован native Nsight GTAPI, trace id при анализе
`gpu-trace-e54c1349dcff8e4b`. В каждой из 16 запрошенных PM-метрик
58136 сэмплов, номинальный интервал 6976 нс; интегрированное покрытие каждой —
405329088 нс. Overflow warnings нет. Единственное предупреждение:
удалены отсутствующие per-shader metrics `CS_0x9ee459d0785be7ca`,
`CS_0x049fb2d2bb33b92d`, `PS_0xfe4c99413cb06935`.
Они не использованы. Shader PC samples = 0; нет измерения stall reasons
и точного числа регистров конкретного скомпилированного шейдера.

В таблице occupancy = сумма VTG + PS + CS hardware counters,
усреднённая по времени пересечения PM-сэмпла с GPU marker.
Агрегированы все 12 экземпляров каждого верхнего прохода;
вложенные маркеры повторно не суммируются.
p95 — процентиль **PM-сэмплов по времени**, не p95 кадров.
Нулевое время маркера означает отсутствие измеримого интервала работы,
его occupancy обозначена «—». Это не оценка выключенной функции.

Отдельная native range-агрегация даёт
3,333108% VTG + 10,354052% PS + 13,848138% CS = **27,535299%**,
что согласуется с интеграцией сэмплов (разница 0,001690 п.п.).
Через native range query SM Throughput = 26,856%, PCIe = 7,003%;
средние сэмплов — 27,396% и 7,818% соответственно.
Эти способы агрегации не подменяются друг другом; таблица использует
средние PM-сэмплов.

## Проходы

| GPU marker | Среднее, мс/кадр | Occupancy mean, % | Occupancy p95, % | SM throughput mean, % |
| --- | ---: | ---: | ---: | ---: |
| Local shadow scene upload | 0.272 | 0.0 | 0.0 | 0.0 |
| Environment | 0.000 | — | — | — |
| Culling | 0.284 | 38.4 | 53.8 | 0.3 |
| Depth prepass | 1.142 | 11.9 | 27.6 | 13.7 |
| Sun shadows | 7.807 | 16.0 | 53.8 | 15.7 |
| Local shadows | 10.137 | 14.9 | 48.7 | 8.1 |
| Reflection probes | 0.000 | — | — | — |
| GBuffer | 2.343 | 7.0 | 20.3 | 15.4 |
| Ambient occlusion | 0.399 | 57.2 | 65.0 | 53.0 |
| Clustered deferred lighting | 1.077 | 59.1 | 66.7 | 73.3 |
| Screen-space reflections | 2.190 | 56.5 | 81.8 | 67.1 |
| Fluid surface | 0.000 | — | — | — |
| Behind-water transparency | 0.755 | 39.6 | 65.8 | 41.4 |
| Behind-water composition | 0.035 | 71.3 | 96.3 | 23.5 |
| Fluid resolve | 0.000 | — | — | — |
| Front transparency | 0.281 | 11.0 | 14.8 | 14.0 |
| Particles | 2.910 | 57.3 | 79.5 | 45.7 |
| Front composition | 1.740 | 68.8 | 71.7 | 69.9 |
| Depth of field | 0.125 | 37.1 | 83.3 | 29.9 |
| Motion blur | 0.555 | 60.9 | 65.2 | 69.6 |
| Reconstruction | 1.626 | 24.8 | 62.3 | 52.9 |
| Tone map | 0.047 | 64.2 | 94.3 | 50.2 |
| Frame generation inputs | 0.000 | — | — | — |

Front composition включает sorted/additive HDR particles: в среднем
1,706 из 1,740 мс. Это не стоимость одного простого fullscreen composite.
Particles и Front composition вместе — около 4,650 мс/кадр.

За весь trace GR Engine Active = **97,51%**, SM Throughput по сэмплам =
**27,40%**, VRAM Throughput = **32,82%**.
Занятый GR engine не означает высокую вычислительную загрузку.
Счётчик Unallocated Warps in Active SMs = **53,75%** общего warp-slot budget;
он показывает пустые слоты на активных SM, а не долю полностью простаивающего GPU.

## Где есть смысл повышать occupancy

1. **CSM и основной геометрический путь — первый эксперимент.**
   Sun shadows + local shadows занимают **17,944 мс, около 53,1% кадра**.
   CSM occupancy 15,95%, G-buffer 6,95%, prepass 11,87%.
   При этом расход 3D register file в CSM в среднем только 9,51%,
   в G-buffer 13,18%: эти средние не подтверждают глобальное насыщение регистров,
   хотя не исключают короткие локальные ограничения.
   VRAM throughput CSM 64,22%, p95 82,07%; prepass 77,70%, p95 82,22%.
   Это делает подачу данных/геометрию более полезной гипотезой, чем
   безусловное снижение числа регистров.

   В актуальном исходнике
   [FrameConstants](../../Engine/Engine.RendererDX12/src/GDX12FrameConstants.cpp)
   InstanceCache/TransformCache/MaterialCache создаются как Upload.
   [DeviceResources](../../Engine/Engine.RendererDX12/src/GDX12DeviceResources.cpp)
   создаёт IndirectCommandsCache как Upload.
   [CSM](../../Engine/Engine.RendererDX12/Include/Engine.RendererDX12/RenderPasses/GDX12CascadedShadowPass.h),
   [main culling](../../Engine/Engine.RendererDX12/Include/Engine.RendererDX12/RenderPasses/GDX12GPUCullingPass.h)
   и [opaque pass](../../Engine/Engine.RendererDX12/Include/Engine.RendererDX12/RenderPasses/GDX12OpaquePass.h)
   продолжают привязывать соответствующие исходные caches.
   Local shadows уже читают DEFAULT mirrors.
   Конкретный A/B: переиспользовать существующие mirrors для CSM, затем отдельно
   для остальных совместимых consumers, сохранив обновления, frame fences и states.
   Следить за временем проходов, PCIe, VRAM и occupancy. Наличие UPLOAD-чтений
   подтверждено кодом; их вклад в каждый stall и ожидаемые миллисекунды не измерены.
   Геометрические vertex/index buffers уже DEFAULT: переносить их повторно не нужно.

2. **Local shadows — уменьшить последовательную работу между faces.**
   2030 views за окно, в среднем 169,17/кадр.
   [Execute](../../Engine/Engine.RendererDX12/Include/Engine.RendererDX12/RenderPasses/GDX12LocalShadowPass.h)
   для каждой face выполняет copy → clear → cull → draw с повторным
   использованием Commands/Counter и переходами состояний.

   | Вложенный scope | Среднее, мс/кадр | Occupancy mean, % |
   | --- | ---: | ---: |
   | Copy | 2,253 | 24,1* |
   | Clear | 0,505 | 10,3 |
   | Cull | 1,957 | 29,5 |
   | Draw | 5,113 | 6,4 |

   *Copy не имеет shader occupancy: это shader residency в совпадающих или
   соседних PM-интервалах. Все эти scopes включают setup/barriers, а короткие
   clear/copy могут делить 7-мкс сэмплы с соседней работой.
   Это не shader-only timing и не точная атрибуция счётчиков отдельному dispatch.

   [Cull shader](../../Engine/Engine.RendererDX12/Shaders/LocalShadowPass.hlsl)
   использует 64 threads/group, проходит общий список 24777 команд и отбрасывает
   static/dynamic класс после чтения instance. ClearCS — один thread в одном group.
   Делать этот clear многопоточным ради occupancy бессмысленно.
   Проверяемые варианты: компактные static/dynamic входы; пакетная очистка
   counters; culling небольшой группы dirty faces с независимыми output segments.
   Это может убрать повторные чтения и переходы, но выигрыш не измерен.
   Нужен ограниченный memory budget; нельзя выделять полный scene-list output
   для каждой face без расчёта памяти или вводить truncation casters.
   Только cull сейчас занимает 1,957 мс: его полное устранение дало бы верхнюю
   арифметическую границу около 5,8% текущего кадра, а не обещание такого ускорения.

3. **Compute/post уже имеют умеренную или высокую occupancy.**
   AO 57,2%, lighting 59,1%, SSR 56,5%, motion blur 60,9%;
   particle stages 57,3% и 68,8%. Здесь нет основания требовать 100%.
   Lighting расходует в среднем 88,71% CS register file, p95 100%;
   его occupancy p95 66,67% — кандидат на отдельную проверку register pressure,
   но не доказанный register launch limiter. Исходный deferred CS использует
   16×16 threads/group; менять размер нужно только отдельным A/B
   с контролем spills, occupancy и времени.
   XeSS 24,8% — агрегат нескольких vendor kernels; их внутренний код
   не является непосредственным местом правки движка.
   DoF/Tone map слишком короткие, чтобы начинать оптимизацию кадра с них.

4. **Async compute — последующий эксперимент.**
   В trace только одна graphics queue, async compute не используется.
   Можно искать независимую compute-работу для перекрытия с геометрией,
   но свободные warp slots не гарантируют свободную memory bandwidth.
   Уже насыщенные интервалы VRAM, зависимости depth/G-buffer и ownership ресурсов
   могут сделать перекрытие бесполезным. Это изменение планирования,
   не бесплатное повышение occupancy.

[NVIDIA определяет occupancy как residency warp slots](https://docs.nvidia.com/nsight-graphics/UserGuide/gpu-trace-system-architecture.html).
[Shader Profiler](https://docs.nvidia.com/nsight-graphics/UserGuide/shader-profiler.html)
показывает, почему дополнительные resident warps не всегда ускоряют shader:
при большом Not Selected допустимо даже уменьшение occupancy.
Для причинного разбора следующего выбранного shader нужен отдельный короткий
PC-sampling trace; для утверждения об ускорении — сопоставимое повторное A/B.

## Артефакты и проверки

Компактные численные результаты, определения колонок, metadata и оговорки:
`Build/Captures/2026-11-03-occupancy/evidence.json` (локально, игнорируется Git).
Raw trace сохранён на прежнем месте; он повторно не записывался.

Проверены 23 группы верхних маркеров по 12 экземпляров, полное интегрированное
покрытие счётчиков и согласованность общей occupancy с native range query.
Для документа выполнены проверка SHA-256 исходного trace,
проверка численной сводки и `git diff --check`.
Сборка и функциональные тесты не запускались: изменён только этот отчёт.
A/B оптимизаций, новый capture текущего HEAD и другие GPU не проверены.

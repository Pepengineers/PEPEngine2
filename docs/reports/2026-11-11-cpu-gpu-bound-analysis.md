# Bistro: CPU/GPU bottleneck analysis

Дата: 11 ноября 2026. Проверенный HEAD: `470bcd904918f6e3f7724e5da88c77acb0e860d7`.

## Вывод

Рабочий диагноз для полной Debug-сцены: смешанное ограничение с приоритетом CPU-подготовки и физики. Снижение разрешения не помогает; физика не успевает за реальным временем. Это не доказательство отсутствия GPU bottleneck: геометрия, shadow maps и compute simulation не масштабируются пропорционально разрешению окна. Для строгого разделения текущей версии нужен совместный CPU/GPU timeline.

## Новая проверка

Использована копия существующего `Build/bin/x64/Debug/App/App.exe` под именем `App.BoundAnalysis.exe`. Сборка заново не выполнялась; соответствие всех объектов текущему HEAD не доказано. SHA256 бинарника: `eb5de81696b4c7a4d5aceb68507ed160bfdfdba86b41e2009a404aff5df6385c`.

Конфигурация: runtime `Config/Engine.ini`, отдельные overlay с `App.Quality.OutputWidth/Height`; прочие параметры не менялись. MGPU выключен. Стартовая неподвижная камера Bistro, seed 12345, 120 warmup + 600 measured frames, обычные real-time physics updates. Это не 100-секундный маршрут Quality. Изменение размера проверяется самим `ResizeQualityClient`; отчёт loading benchmark не содержит resolved GPU timestamps.

| Порядок | Размер | мс/кадр | FPS | Долг физики начало → конец, с | Выполненные ticks |
|---|---|---:|---:|---:|---:|
| A1 (`high2`) | 1280×800 | 58,6719 | 17,04 | 2,9007 → 10,2847 | 1666 |
| B (`low1`) | 768×480 | 59,4528 | 16,82 | 2,2889 → 9,7208 | 1690 |
| A2 (`high3`) | 1280×800 | 60,4148 | 16,55 | 2,5134 → 11,0723 | 1658 |

На 64% меньше пикселей: B практически совпадает со средним A1/A2 (59,5434 мс; отличие около 0,15%). Разброс A1/A2 около 3%; ускорение от разрешения не установлено. Физика выполнила 27,63–28,17 с симуляции за 35,15–36,19 с запрошенного времени: примерно 76–79%. Среднее FPS само по себе скрывает это отставание. GPU allocations составили около 3384/3293/3384 МиБ; это не пик VRAM и не доказательство отсутствия residency stalls.

Все три запуска завершились кодом 0; runner подтвердил неизменность executable, INI и соответствующего overlay. Raw JSON, per-frame samples, DOT и fingerprints: `Build/BoundAnalysis/{high2,low1,high3}*` (локальные, ignored). Сначала был ошибочный запуск `high1`: PowerShell разбил незакавыченные аргументы с точками, приложение работало интерактивно до timeout. Он полностью исключён из сравнения. Телеметрия `gpu-high1.csv` относится только к нему, не к успешным A/B/A. В корректной серии частоты/температуры не записывались, thermal drift не исключён. Измерения не являются независимыми повторами на одинаковых физических состояниях: real-time шаги дают разные состояния и число ticks.

## Что подтверждает код

- `Apps/App/src/App.cpp`, `BistroApp::Update`: последовательные зависимости movement → circle → spline → look-at → lights, затем `Complete`, physics и GPU data update с ещё одним `Complete`. Нельзя просто убрать ожидания: следующие стадии читают результаты предыдущих.
- `Apps/App.Base/src/App.cpp`, `App::UpdatePhysics`: шаг 1/60 с, не более 8 ticks за кадр; выход после превышения примерно 16,7 мс wall budget. Проверка после шага допускает превышение бюджета на стоимость последнего шага. Если кадр занимает 59 мс, нужно в среднем 3,5 ticks; новая серия выполняет около 2,8. Долг закономерен.
- `Apps/App.Base/src/RenderModule.cpp`, snapshot около строки 1305: каждый кадр обходятся static-mesh components и submeshes, читаются материалы, сравниваются matrices/instances. Кэш уже избегает части пересчётов, но не общего обхода. Ниже также есть проход по primitives для caster/reflection state.
- Там же, `OnRender`: skeletal preparation, material updates, particle collection, pipeline preparation и graph recording. `OnUpdate` ждёт только занятый frame-resource slot, а не обязательно предыдущий кадр.
- `Apps/App/src/Rendering/Water/GDX12Fluid.cpp`: отдельные hash/sort/density/integration compute stages; уменьшение окна не убирает particle simulation.
- `Engine/Engine.RendererDX12/src/GDX12GPUTimer.cpp`: timestamp interval и frame wall time различаются. При split submissions GPU interval может включать ожидания/провалы между submissions; его нельзя автоматически называть временем активной работы всех GPU units.

## Историческая атрибуция — не свежие тайминги

[CPU-профиль 8 ноября](2026-11-08-cpu-sync-batching.md): CPU frame 57,269 мс, GPU interval 48,961 мс; physics loop 18,751 мс, включая Jolt 16,229 мс; snapshot 8,987 мс; skeletal preparation 5,078 мс; pipeline prepare 15,493 мс; graph CPU recording 10,018 мс. Интервалы вложены и перекрываются — складывать их нельзя. С тех пор менялся код, включая barrier filtering в текущем HEAD.

[Проверка scheduling](2026-11-08-render-scheduling-hypotheses.md) ранее обнаружила 0 frame-slot waits в 1800 измеренных кадрах. Поэтому четвёртый кадр в очереди или перенос fence wait не имеют доказанного резерва для этой нагрузки.

## Порядок оптимизации

1. **Сначала обновить атрибуцию.** Записать CPU scopes и GPU queue timeline на одном коротком интервале текущей сборки: snapshot, skeletal, physics backend/sync, prepare, graph recording, Present и fence waits. Сохранять clocks/thermal flags и physics debt. Проверить Release отдельно: текущие цифры относятся только к Debug. Это база для выбора изменения, а не обещание прироста.
2. **Snapshot и render preparation.** Проверить возможность обхода только изменившихся components/instances вместо полной проверки static geometry. Использовать существующие revision/dirty механизмы и кэши; сохранить invalidation при изменениях transform, mesh, material, visibility и удалении entity. Критерий: меньше CPU-времени snapshot, одинаковые primitives/bounds/shadows и итоговый кадр.
3. **Jolt integration и публикация поз.** Разделить backend, scheduler и pose sync; проверить обработку active/changed bodies, гранулярность jobs и число workers на одинаковом движущемся состоянии. Batch 256 уже внедрён и ранее проверен — не выдавать его за новое решение. Сохранить 60 Гц, CCD, solver и формы; не маскировать overload сбросом accumulator. Приёмка: долг не растёт в длительном real-time прогоне.
4. **Skeletal preparation и command recording.** Найти повторную подготовку одинаковых poses, allocations и повторные scans по views/passes; переиспользовать существующий instancing и кэши. Последнюю оптимизацию barrier filtering сначала измерить, а не переписывать повторно. Параллельная запись допустима только после проверки независимости pass data и allocator ownership.
5. **GPU после capture.** Разделить local/sun shadows, grass expansion в shadow views, water sort/density, reflections и post effects. Для теней проверить reuse статических casters и обновление только изменившихся lights/casters; для воды — число dispatch и размер сортируемого набора. Уменьшение shadow resolution, counts, частоты обновления или качества — отдельные варианты с визуальным компромиссом, не эквивалентная оптимизация.

Async compute/MGPU не включать вслепую: выигрыш требует реального перекрытия, а transfers/fences могут съесть его. Перенос физики на отдельный поток требует согласованных snapshots и ограниченной очереди, не устраняет её вычислительную стоимость. Замена физического движка пока не обоснована сравнительным измерением. Простое снижение разрешения на данном ракурсе уже проверено и не помогло.

Изменений production-кода нет. Функциональные тесты не запускались: изменён только этот отчёт. Не проверены Release, полный маршрут, другие камеры/профили, актуальные per-pass GPU times и устойчивость к нагреву в длительном тесте.

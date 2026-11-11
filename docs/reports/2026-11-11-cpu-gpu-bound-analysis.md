# Bistro: CPU/GPU bottleneck analysis

Дата: 11 ноября 2026. Проверенный HEAD: `470bcd904918f6e3f7724e5da88c77acb0e860d7`.

## Вывод

После проверки PIX: **CPU-bound на измеренном рабочем участке полной Debug-сцены**. CPU выдаёт кадры примерно раз в 65,7 мс, размеченный GPU-пакет занимает около 43,3 мс. GPU имеет заметный запас до темпа CPU; после оптимизации CPU следующим ограничителем могут стать вода и тени. Исходная проверка только изменением разрешения давала предварительный диагноз; ниже она сохранена как дополнительное свидетельство.

## Проверка PIX по запросу пользователя

Первый успешный timing capture: `Build/BoundAnalysis/bistro-timing.wpix`, 142 389 248 bytes. PID 12960, main TID 31336; PIX 2610.28. Manifest: 0 lost ETW events/buffers, 8 CPU event definitions, 1272 CPU executions, 50 GPU event definitions, 3 API queues (все PID 12960). Вопреки общей подсказке capture wrapper, attach-захват содержит и CPU scopes, и GPU timings: это проверено manifest и самими executions. Файл прочитан штатным `PixTimingCapturesMcp.exe` через локальный MCP-клиент; динамически включённые query tools не появлялись в интерфейсе чата.

Анализируется диапазон 1000–10000 мс на часах capture, после загрузки сцены. Ответы PIX дополнительно фильтровались локально по полному попаданию события в диапазон: запросы по event ID возвращали также строки вне указанного окна. CPU/GPU scopes вложены, поэтому parent/child не суммировались. Между 136 началами `Game update` средний интервал 65,684 мс. Для 136 полных GPU-пакетов от `Local shadow scene upload` до `Tone map`: средняя длина 43,289 мс, промежуток до следующего начала 22,450 мс (минимум 9,745 мс). Это промежутки между размеченными пакетами, не аппаратный счётчик SM utilization; неразмеченный UI/Present tail в длину пакета не входит.

| CPU-маркер | Среднее, мс | Полных событий |
|---|---:|---:|
| Game update | 26,965 | 136 |
| Scene preparation | 16,069 | 137 |
| ↳ Scene snapshot | 10,137 | 137 |
| ↳ CPU light grid | 1,506 | 137 |
| ↳ Shadow visibility wait | 0,455 | 137 |
| Legacy graph split recording | 13,675 | 137 |
| Submit | 0,446 | 137 |
| Frame slot wait and upload, внутри Game update | 0,0207 | 136 |

| GPU-маркер | Среднее, мс/выполнение |
|---|---:|
| Fluid simulation | 13,194 |
| Local shadows | 13,119 |
| Sun shadows | 6,765 |
| Particles | 1,690 |
| Conditional composition | 1,360 |
| GBuffer | 1,248 |
| Screen-space reflections | 1,094 |
| Reconstruction | 0,922 |
| Depth of field | 0,123 |

Число событий GPU в этом окне 136–137 для перечисленных проходов. Некоторые другие markers вызываются дважды за кадр; их среднее на вызов не выдаётся за стоимость кадра. В захвате есть вложенный `XeSS HDR Upscale`; данный профиль нельзя называть проверкой native rendering только по размеру окна. Маркеры `Frame generation inputs` имеют нулевую длительность и сами по себе не доказывают активную генерацию кадров.

Локальный `App.pdb` успешно разрешил стеки. Из 7058 CPU samples main thread декодированы 450 наиболее частых stack IDs, покрывающих 2077 samples: подтверждены `RenderModule::CollectReflectionScene`, обход `StaticMeshRenderComponent`, `App::UpdatePhysics`, `JoltPhysicsBackend::Step` и `JPH::PhysicsSystem::Update`. Это смещённая подвыборка, проценты функций по ней не оцениваются; отдельное актуальное время Jolt из Game update не выделено. Context-switch stream также выгружен (49170 записей), но полной атрибуции ожиданий по нему не делалось.

Второй файл `bistro-launched-timing.wpix` исключён: в нём одновременно оказались старый PID 12960 и новый PID 31724, причём новый процесс успел только один Game update. Остановка старого процесса из ограниченной оболочки не сработала; затем собственные диагностические PID проверены/остановлены вне sandbox. Первый capture записан раньше второго запуска. Первый неуспешный attach к PID 37384 также не дал измерений. Эти попытки не входят в таблицы.

**Уточнённые действия:** сначала уменьшать 10,14 мс snapshot и 13,67 мс CPU graph recording; отдельно детализировать 26,97 мс Game update и Jolt. GPU-приоритет — вода, затем локальные/солнечные тени, а не DoF/AO. В `GDX12FluidSolver` каждый substep делает bitonic sort по Capacity с вложенными k/j и UAV barrier после каждого dispatch: проверить стоимость этой цепочки и вариант сортировки с меньшим числом глобальных проходов, сохранив соседей/детерминизм. Один лишь PIX scope Fluid simulation не доказывает, что именно sort занимает все 13,19 мс. Для теней проверить число реально обновляемых граней и повторные clear/cull/draw/grass expansion, используя уже существующее кэширование, а не снижая качество вслепую. 43,3 мс GPU-пакета означает, что оптимизация только CPU также не обеспечит 60 FPS на этом профиле.

Это один инструментированный Debug-интервал; абсолютные значения не эквивалентны ordinary benchmark. Release, другие камеры и длительный thermal soak не проверены. Production-код не менялся.

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

## Первоначальный порядок оптимизации (до PIX; уточнение выше)

1. **Сначала обновить атрибуцию.** Записать CPU scopes и GPU queue timeline на одном коротком интервале текущей сборки: snapshot, skeletal, physics backend/sync, prepare, graph recording, Present и fence waits. Сохранять clocks/thermal flags и physics debt. Проверить Release отдельно: текущие цифры относятся только к Debug. Это база для выбора изменения, а не обещание прироста.
2. **Snapshot и render preparation.** Проверить возможность обхода только изменившихся components/instances вместо полной проверки static geometry. Использовать существующие revision/dirty механизмы и кэши; сохранить invalidation при изменениях transform, mesh, material, visibility и удалении entity. Критерий: меньше CPU-времени snapshot, одинаковые primitives/bounds/shadows и итоговый кадр.
3. **Jolt integration и публикация поз.** Разделить backend, scheduler и pose sync; проверить обработку active/changed bodies, гранулярность jobs и число workers на одинаковом движущемся состоянии. Batch 256 уже внедрён и ранее проверен — не выдавать его за новое решение. Сохранить 60 Гц, CCD, solver и формы; не маскировать overload сбросом accumulator. Приёмка: долг не растёт в длительном real-time прогоне.
4. **Skeletal preparation и command recording.** Найти повторную подготовку одинаковых poses, allocations и повторные scans по views/passes; переиспользовать существующий instancing и кэши. Последнюю оптимизацию barrier filtering сначала измерить, а не переписывать повторно. Параллельная запись допустима только после проверки независимости pass data и allocator ownership.
5. **GPU после capture.** Разделить local/sun shadows, grass expansion в shadow views, water sort/density, reflections и post effects. Для теней проверить reuse статических casters и обновление только изменившихся lights/casters; для воды — число dispatch и размер сортируемого набора. Уменьшение shadow resolution, counts, частоты обновления или качества — отдельные варианты с визуальным компромиссом, не эквивалентная оптимизация.

Async compute/MGPU не включать вслепую: выигрыш требует реального перекрытия, а transfers/fences могут съесть его. Перенос физики на отдельный поток требует согласованных snapshots и ограниченной очереди, не устраняет её вычислительную стоимость. Замена физического движка пока не обоснована сравнительным измерением. Простое снижение разрешения на данном ракурсе уже проверено и не помогло.

Изменений production-кода нет. Функциональные тесты не запускались: изменён только этот отчёт. Не проверены Release, полный маршрут, другие камеры/профили и устойчивость к нагреву в длительном тесте. Актуальные per-pass GPU times впоследствии получены в PIX и приведены выше.

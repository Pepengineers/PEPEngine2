# Bistro: CPU/GPU attribution после принятого ревью

Дата: 2026-11-12. Это диагностика принятого состояния, а не A/B оптимизации. Изменения MGPU, сделанные параллельно после заморозки, в измеряемый код не включались.

## Результат

В измеренном стационарном real-time сценарии и наблюдавшемся окружении основной предел находится на CPU: кадр 21.50–24.18 мс, завершённый GPU-интервал 16.80–16.98 мс. На CPU наиболее дорогой участок — Jolt backend, 13.97–16.59 мс на кадр; запись и подготовка рендера занимают ещё 6.77–6.87 мс. Present и ожидание GPU fence малы. Эти wall durations включают вытеснение потока и ожидание параллельных jobs: они пока не разделяют полезную работу Jolt и накладные расходы планировщика. Приписать всё измеренное wall time исключительно коду движка нельзя.

При ровно одном физическом тике на кадр два запуска дали 18.53 и 22.79 мс при GPU 16.98 и 16.77 мс. Первый близок к границе CPU/GPU, второй ограничен CPU. Нельзя выбрать лучший запуск и объявить весь fixed-step сценарий GPU-bound. Разброс требует дальнейшего исследования Jolt/jobs и идентичности состояния симуляции.

На GPU крупные участки — Fluid simulation, Local shadows и Sun shadows. Nsight подтверждает их локализацию, но его интервалы с `time_every_action` не заменяют обычные timestamp-замеры. Холодный запуск локализован в CPU VHACD; его исходное elapsed time загрязнено внешней сборкой и диагностическим sampler, поэтому чистый выигрыш cold→warm здесь не заявляется.

## Измеряемая версия и условия

- Принятый Git anchor: `7acaf95089541909925d8e305fa3b0ba55ff5cc8`. Исходные байты 556 файлов зафиксированы в `Build/PerformanceAnalysis/sources.json`. Это точная рабочая копия на момент заморозки, включая существовавшие посторонние изменения теста и метаданных External project; не утверждается, что весь checkout был чистым.
- Для profiling создана обычная копия `Build/PerformanceAnalysis/accepted`, не worktree. `accepted-manifest.json` проверяет каждый исходный hash; девять уже изменённых параллельно файлов восстановлены в этой копии из anchor с проверкой исходных байтов. Общие External/Assets и зависимости подключены junction; их обновления не выполнялись.
- Обычный Release x64 собран успешно за 77.31 с. `runtime/BistroAcceptedRelease.exe` SHA256: `4263cb31ddd7ddf4106e76329e44827fe50e771b0769cfe20d3dcd1ab7e5174d`.
- Profiling — Release `/O2 /GL /MD`, `PEPEnableProfiling=true`, отдельные диагностические TU. Текущий `runtime/BistroProfile.exe` v2 SHA256: `bd6210f2b0f77bfdb32f80ab149a635bee49a37f3d01022d72fc912be5f11fd4`. Предыдущий v1 сохранён как `BistroProfile-v1.exe`, SHA256 `e70b8621b32437ac77852144dc76ede2a67358c75e9e26b4e43aee38526625a1`. Привязку конкретного запуска определяют его fingerprints, а не текущий файл по имени.
- Ryzen 9 5950X, 16 cores / 32 logical processors; RTX 4080 SUPER, 16376 MiB по NVIDIA-SMI, driver 610.88. GPU clocks не фиксировались. D3D debug layer выключен, аргумент `-d3ddebug` не передавался.
- Конфигурация скопирована без снижения качества и количества объектов; явно задан только размер вывода 1280×800. Hash `runtime/Config/Engine.ini`: `67fa80bc81bc18c878ced8c7715c81583bdde62b5010f34b455667101600107d`.
- Runtime: XeSS, render 985×616, output 1280×800; FG requested/effective Off, VSync Off. Статичная камера (-300, 200, 20), rotation (0, 117, 0), seed 12345. 949 rigid bodies, 10 людей, 5 лис, 100980 начальных водных частиц, capacity обычных particles 500000. Reflection probes 4/4 готовы в начале и конце measurement.
- Real-time physics: dt 1/60, максимум восемь тиков за кадр, wall budget 1/60, накопленный долг сохраняется. Волны зависят от времени симуляции. Поэтому равное число кадров в real-time не означает равное количество физической работы.
- 120 warmup frames, аргумент `BenchmarkFrames=1201`; экспорт содержит ровно 1200 измеряемых кадров. CPU CSV использует измеряемый диапазон с offset 121. GPU totals снимаются после drain на границах, count также ровно 1200. Вложенные CPU scopes нельзя складывать между собой.

## Обычные и диагностические кадры

Все значения ниже — мс; frame — wall-time из штатного benchmark result, GPU — сумма завершённых GPU timestamp-интервалов / 1200. У обычного Release дополнительной GPU-телеметрии нет.

| Run | Режим | Frame mean | p50 | p95 | GPU mean | Physics ticks | Simulated seconds |
|---|---|---:|---:|---:|---:|---:|---:|
| quiet-1 | Ordinary Release, real-time | 23.9832 | 23.6082 | 35.5431 | — | 1724 | 28.7333 |
| profile-scope-2 | Profiling, real-time | 24.1768 | 24.0044 | 36.5870 | 16.9792 | 1743 | 29.0500 |
| profile-scope-3 | Profiling, real-time | 21.5011 | 19.2561 | 33.7355 | 16.7973 | 1550 | 25.8333 |
| profile-fixed-1 | Profiling, fixed 1/60 | 18.5262 | 17.9784 | 24.1403 | 16.9847 | 1200 | 20.0000 |
| profile-fixed-2 | Profiling, fixed 1/60 | 22.7864 | 21.5274 | 34.1881 | 16.7669 | 1200 | 20.0000 |

`PERF_FIXED_STEP=1` существует только в диагностической копии App: каждый benchmark tick получает 1/60. Fixed runs имеют requested=simulated=20 с и debt=0. Они нужны для отделения количества тиков от wall-time; это не изменение штатного игрового времени и не доказательство побитно одинаковой траектории Jolt. Полный state hash в этой серии не записывался.

| CPU scope, mean ms | scope-2 | scope-3 | fixed-1 | fixed-2 |
|---|---:|---:|---:|---:|
| Instrumented frame | 24.1547 | 21.4819 | 18.5064 | 22.7656 |
| Physics whole | 16.7912 | 14.1428 | 11.1530 | 14.3485 |
| Jolt backend, nested in physics | 16.5913 | 13.9747 | 11.0187 | 14.1939 |
| Dynamic transform sync, nested in physics | 0.1160 | 0.0973 | 0.0778 | 0.0888 |
| Render whole | 6.8712 | 6.7667 | 6.9233 | 7.9660 |
| Graph recording, nested in render | 4.7452 | 4.7753 | 4.8513 | 5.5276 |
| Pipeline prepare, nested in render | 1.0370 | 1.0265 | 1.0439 | 1.1732 |
| Present | 0.2712 | 0.2478 | 0.2590 | 0.3153 |
| Slot fence wait | 0.0705 | 0.1806 | 0.0328 | 0.0000 |

Instrumented frame немного короче штатного wall interval: границы разные. GPU interval — длительность от timestamp начала до конца GPU frame, не сумма аппаратно активных тактов. CPU и GPU перекрываются; их времена не складываются в frame.

Real-time debt start→end: quiet-1 0.037281→0.062971 с, scope-2 0.007314→0.008324 с, scope-3 0.004621→0.000927 с. Эти серии не показывают устойчивого многосекундного накопления долга. Контаминированный warm-1 имел 5.02 с долга; использовать его как доказательство внутреннего дефекта physics budget нельзя.

## GPU attribution и ограничения capture

Nsight Graphics 2025.5: восемь кадров после `start_after_frames=180`, `time_every_action`, `AsyncComputeTriage`, clocks unaltered, auto-export. Исходные `.ngfx-gputrace` сохранены; экспортные `.xls` фактически TSV. Успешный export заканчивается штатным завершением captured process; trailing connection error после `Terminating process` не означает провал записи. Обычного benchmark result у этих capture runs нет.

| Parent marker, mean ms | Исходный trace, 02:34 Istanbul | Контрольный trace, 14:54 Istanbul |
|---|---:|---:|
| Fluid simulation | 4.7318 | 5.5161 |
| Local shadows | 4.6452 | 6.0897 |
| Sun shadows | 3.1477 | 3.9947 |

Исходный trace: frame intervals 25.67–37.92 мс, graphics-active estimate 16.15–16.28 мс, сумма parent markers 16.30–16.42 мс. Было 38 наблюдений процессов сборки. Это источник локализации, не чистая baseline производительности.

Контрольный trace: frame intervals 26.24–40.24 мс, graphics-active estimate 20.03–23.32 мс, сумма parent markers 19.72–20.95 мс. Компиляторы не обнаружены в 92 снимках. Однако к этому времени был открыт пользовательский Unreal Editor; исходный monitor его не охватывал. Другой участок симуляции, capture overhead, GPU clocks и внешняя нагрузка не изолированы. Разницу между двумя traces нельзя приписать конкретной причине или оптимизации.

Fluid L2 throughput в исходном capture около 84–85% peak, при существенно меньших SM/DRAM counters; у Sun shadows высокий DRAM throughput. Эти счётчики задают направления проверки, но сами не доказывают bandwidth-bound причину. В control capture Fluid L2 66.8–85.0%, SM 17.2–22.1%, DRAM 2.0–7.6%. Нужна стадийная разбивка полного SPH. `gr__dispatch_count` не равен числу API Dispatch или вызовов solver Record.

В обоих exports найдена некорректная вложенная строка `Local shadow draw`: row 665 исходного trace и row 684 контрольного имеют значения длиннее кадра и parent. Каждая строка исключена целиком, включая её правдоподобные ранние значения. Parent timings проходят sanity checks границ; это не устраняет ограничения инструментального измерения. REGIMES содержит повторяющиеся parent occurrences; для восьми frame counters используется первое occurrence, последующие не перезаписывают его. Подробный разбор теней принят отдельно: [shadow-performance](2026-11-12-shadow-performance.md), commit `d19cbf47b590203e41b61bd047b727baf369e470`. Изменений теней и подтверждённого ускорения в том отчёте нет.

## Холодная загрузка

`cold-stacks`: ready 78.079 с, initialization 71.405 с, scene publish 59.293 с. Mesh import 1.122 с и texture stage 2.829 с не объясняют основную задержку. Эти elapsed values сохраняются как наблюдение, но исключены из speed acceptance: с 23:22:23 до 23:22:38 UTC выполнялась внешняя renderer build, sampler кратковременно приостанавливал отдельные потоки.

В ранних 213 stack snapshots (71 поток × 3 раунда) 93 worker stacks принадлежат VHACD; main ждёт `App::WaitForLoading → CreateBistroPropEntities::BakeColliders`. Между раундами 79.7 CPU-seconds прироста принадлежат worker threads с цепочкой `BuildDynamicCollision → VHACD::PerformConvexDecomposition → PerformPlaneSplit → VoxelHull::BuildRaycastMesh → AABBTree::BuildRecursive`. Частые верхние frames — `FaceSorter/std::partition` и `CalculateFaceBounds`. После публикации VHACD исчезает из поздней выборки. Это CPU collision cooking, не GPU fence.

Sampler не является статистическим CPU profiler: он последовательно снимает стеки с краткой остановкой отдельных потоков, первая символизация дала максимум 593 мс suspend, суммарно около 1.015 с. Число snapshots не переводится в процент CPU. Self-check busy/waiting stack fixtures прошёл: 15 stacks, 0 errors.

После cold run созданы 42 collision cache files. Warm scene publish в наблюдавшихся runs около 2.10–2.58 с, но чистый cold/warm speedup требует парного измерения с контролем cache и внешней нагрузки. Отдельная collision-задача проверяет пропуск неиспользуемого raycast AABB tree при `m_findBestPlane=false`; исходный VHACD header SHA256 `c2b750a41bc6044457c79dc51d196b7bbe5e24bfaec72d409a065434d9651ff2`.

Контаминированные frame stack snapshots дополнительно показывают main в Jolt CreateJob/QueueJobs/enki wake/barrier и CPU shadow recording. Восьми main snapshots недостаточно, чтобы количественно обвинить `ReleaseSemaphore` или выбрать оптимизацию scheduler.

## Исключённые данные и контроль среды

- `warm-1`: внешняя App.Base build, frame 32.04 мс и debt 5.02 с исключены из вывода о штатной производительности.
- `warm-2/3`: 24.36/24.71 мс, но нет полной process timeline; только предварительное подтверждение масштаба.
- `quiet-2`: compiler hits 51/60; `quiet-3`: 1/47. Не включены в чистую от обнаруженных сборок выборку.
- `profile-scope-1`: compiler hits 63 плюс stack sampler; timing исключён, stacks используются только качественно.
- `quiet-1`, `scope-2/3`, `fixed-1/2`: соответственно 47/47/43/39/45 process snapshots, compiler hits 0. Все прошли fingerprint-before/after проверку. Эти измерения выполнены 2026-11-11 23:34–23:45 UTC.
- Process monitor опрашивал cl/link/MSBuild, Bistro/App/tests, ngfx, stacks и python примерно раз в секунду. Ноль совпадений означает отсутствие обнаруженных сборок, а не доказанную тишину всех приложений или отсутствие очень короткого процесса между samples. GPU sensors писались отдельно.
- Позже обнаружен UnrealEditor PID 36404, start 2026-11-12 08:57:25 UTC: этот конкретный процесс не мог влиять на перечисленные ночные runs, но существовал во время дневного control capture и первых follow-up collision timings. Его CPU/GPU влияние исходная timeline не измеряла. Пользовательский Editor не останавливался.

## Артефакты и воспроизведение

Все сырые данные находятся локально в `Build/PerformanceAnalysis/` (ignored): `sources.json`, `accepted-manifest.json`, `cpu.json`, `gpu.csv`, build logs; для каждого run — `*-launch.json`, state `*.json`, `*-result.json`, `*-processes.jsonl`, `*-sensors.csv`, `*-engine.log`. Profile дополнительно пишет `*-result.json.cpu.csv` и `*-result.json.gpu.json`. Сводки: `runs-summary.json`, `trace-summary.json`, `gputrace-control-summary.json`, `cold-stack-summary.json`.

Trace files:

- `gputrace/BistroProfile_2026_11_12_02_34_05.ngfx-gputrace` и `gputrace/BASE_UNLOCKED/`.
- `gputrace-control/BistroProfile_2026_11_12_14_54_01.ngfx-gputrace` и `gputrace-control/BASE_UNLOCKED/`.

Повторный анализ без запуска приложения:

```powershell
python Build/PerformanceAnalysis/analyze_runs.py
python Build/PerformanceAnalysis/analyze_trace.py
python Build/PerformanceAnalysis/analyze_trace.py gputrace-control
```

`analyze_runs.py` проверяет exit/status и fingerprints, 1200 кадров/GPU samples, соответствие физических ticks, равенство debt balance с точностью округления JSON; fixed runs требуют ровно 1200 ticks, 20 секунд и нулевой долг. Trace parser проверяет восемь кадров, положительные parent durations и сумму parent ниже frame interval; ошибочные nested rows явно перечислены. Подготовка instrumentation проверяет уникальность замен и link object substitutions. Оба анализа выполнены успешно.

Для нового запуска после выделения окна можно использовать `python Build/PerformanceAnalysis/run.py profile-scope-NEW 120 1201 300` или `profile-fixed-NEW` для диагностического fixed-step. Имена должны быть новыми. Обычный frozen EXE содержит shader paths исходного checkout; после MGPU-правок повторять его как тот же baseline без восстановления проверенных shader bytes нельзя. Profiling EXE использует frozen accepted shader paths. Existing accepted libraries собраны с profiling и не должны использоваться как ordinary Release baseline.

## Переданные задачи

| Чат | Scope и критерий | Состояние при выпуске отчёта |
|---|---|---|
| `01a1ccf6-4da8-7b22-9eb0-ff08ae87dd23` | VHACD startup: сохранить геометрию/cache bytes, true/false findBestPlane и sync/async; отдельные cold/warm ordinary A/B | 12 correctness cases и 42 cache files совпали; timing acceptance продолжается |
| `01a1ccfc-343f-75a3-8b24-3c829266a1d8` | Полный SPH: реальные Record/count/substeps, по стадиям, bitwise oracle, минимум три пары, не выдавать sort-only gain за solver/FPS | Окно выделено, выполняются attribution/correctness и первые парные проверки |
| `01a1ccfd-7c09-7fa3-a9f7-07cff37c73b5` | Тени: только диагностика в Build и отчёт; shared shadow files не менять до согласования после MGPU | Diagnostic report принят и закоммичен, ускорение не заявлено |
| `01a1cf99-fa8e-7383-881a-3a9628a25e73` | Jolt/jobs: разделить main enqueue/wake/barrier и worker bodies; fixed-work A/B, state correctness, overhead instrumentation | Harness подготовлен, ждёт окна после SPH |

Сборки и benchmark/capture окна этих задач сериализуются координатором. Независимый MGPU-чат не управляется; его процессы и изменения только наблюдаются. Правки передаются замороженным READY reviewer-у с точными paths/hashes; локальные коммиты делает reviewer. Этот отчёт не заявляет нового ускорения: он фиксирует измеренный предел и проверяемые направления работы.

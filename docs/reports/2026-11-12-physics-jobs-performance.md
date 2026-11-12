# Jolt jobs: атрибуция wake и проверка пакетной постановки

Дата: 2026-11-12. Статус: **диагностический результат; production-оптимизация не принята и не применена**.

## Результат

В адаптере `JoltJobSystem::QueueJobs` каждый готовый job отдельно передаётся в enkiTS. Измерение локализовало значительную часть main-thread critical path в `WakeThreadsForNewTasks`. Подготовленный в `Build` прототип передаёт готовый пакет одним параллельным `ITaskSet`, сохраняя одиночный `QueueJob`, все jobs и физические настройки.

Две завершённые пары без глубоких job-probes показали снижение wall-time backend на 54.7–67.5%, frame на 7.6–17.7%, без регрессии измеренных p95 и прогрева. Это **условный сигнал**, не законченная acceptance-серия: требовались три пары, третья остановлена после сворачивания окна baseline. Внешние git-процессы создавали различную CPU-нагрузку. Чистый overhead instrumentation, uncontended gain и обычный real-time результат кандидата не установлены.

Функциональные проверки пройдены: специализированные scheduler/lifetime tests и существующие JobSystemTests; хеши и длины полного Jolt SaveState, active/total bodies совпали на всех 1321 тиках baseline/candidate, включая прогрев. Производственные файлы не менялись. Не следует применять прототип лишь на основании двух удачных пар.

## Основа эксперимента

- Accepted anchor: `7acaf95089541909925d8e305fa3b0ba55ff5cc8`, источники `Build/PerformanceAnalysis/accepted`, manifest из исходного [perf-отчёта](2026-11-12-performance-analysis.md). Текущий shared checkout и его MGPU-изменения в executable не включались.
- Ryzen 9 5950X, 32 logical processors; RTX 4080 SUPER, driver 610.88. Исходные accepted Release `/O2 /GL /MD` libraries собраны с profiling. Это не обычный Release без PIX.
- Exact profile reference скопирован в собственный runtime; SHA256 совпадает с исходным `BistroProfile.exe`. Normal candidate использует те же profile App/render objects и accepted libraries; единственная замена — объект `JoltJobSystem.cpp`.
- Во всех A/B одинаковые верхнеуровневые FrameProbe и GPU timestamps. **В normal A/B отсутствуют JobsProbe, QueryThreadCycleTime и SaveState.** Deep diagnostic/state executables отдельные.
- Frozen accepted shaders, одинаковые DLL/config, output 1280×800, XeSS render 985×616, FG Off, статичная камера (-300,200,20)/(0,117,0), seed 12345. 949 rigid bodies, 10 people, 5 foxes, 100980 water particles, particle capacity 500000; probes 4/4.
- `Engine.ini` SHA256 `67fa80bc81bc18c878ced8c7715c81583bdde62b5010f34b455667101600107d`. У каждого запуска сохранены fingerprints до/после. Failed runs также сохраняются.
- По 120 warmup frames, затем 1200 measured frames, 1200 physics ticks, 20 simulated seconds, debt=0. Общий state oracle содержит 1321 tick. Количество тел, shapes, solver settings, dt, time scale, sleeping и число выполняемых jobs не урезались.
- Штатный контракт App::UpdatePhysics сохранён: fixed step 1/60, максимум 8 ticks, wall budget 1/60, удержание долга. Диагностическое `PERF_FIXED_STEP` уже существовало в frozen profile App; оно не меняет production clock.
- CPU/GPU/build окна выдавал perf-координатор. Чужие процессы и окна не останавливались, MGPU-чат не управлялся.

## Атрибуция

Первый deep probe измеряет Create, Queue, QueueJobs, enki wake, Jolt barrier, Drain и тело каждой именованной Jolt функции. Вложенные интервалы вычитаются на том же потоке; main и сумма workers записываются отдельно. Barrier исполняет полезные jobs самостоятельно: его inclusive duration нельзя называть ожиданием. Даже exclusive remainder содержит обход barrier и ожидание, а не только sleep.

`fixed-probe-1`: Jolt Update 11.9205 мс/tick. Main exclusive wake 6.4700, barrier remainder 3.0721, Create 0.1998, Queue 0.0700; main FindCollisions 0.6303, SolveVelocityConstraints 0.8590, SolvePositionConstraints 0.3830. Неинструментированный остаток main — 0.1503 мс. Эти интервалы включают preemption.

Около 548 jobs/tick ставились в очередь; около 465 из них приходили через примерно 26.5 вызовов QueueJobs, которые разворачивались в отдельные enqueue/wake. Суммы worker durations перекрываются между потоками и **не добавляются к main/frame**.

Дополнительная диагностическая пара с `QueryThreadCycleTime` и SaveState:

| Показатель на tick | Baseline `state-cycles-1` | Batch `state-batch-retry` |
|---|---:|---:|
| Jolt Update wall, мс | 13.2117 | 5.2027 |
| Main wake wall, мс | 5.9483 | 0.9239 |
| Main wake cycles | 19,860,726 | 3,150,638 |
| Worker wake cycles, сумма | 21,699,161 | 6,200,465 |
| Main wake calls | 139.52 | 25.96 |
| Worker wake calls | 408.93 | 182.01 |
| Выполненные тела jobs, main+workers | 548.45 | 548.41 |

Cycles — сырые счётчики выполнения потока; в миллисекунды не переводились. Они также включают накладные расходы probes. Разница числа внутренних Jolt jobs мала, но не замалчивается: распределение работ зависит от scheduling; выполненные physics ticks и state hashes совпали. Enki может дополнительно будить workers при дроблении украденных диапазонов, поэтому один batch submission не означает строго один wake на весь пакет.

Deep wrapper копирует `std::function`, увеличивает instrumentation cost. В первом сравнении exact profile/deep frame был 21.2962/21.2309 мс, но внешний git занимал примерно 2 ядра в reference и более 5 в deep. Это **не** доказательство нулевого overhead. Для speed evidence ниже использованы executable без этих probes.

## Normal fixed-work пары

Объявленный порядок: A1/B1, B2/A2, A3/B3. Все завершённые samples сохранены, выбор лучшего запуска не выполнялся. В таблице wall-time в миллисекундах; backend вложен в frame.

| Run | Frame mean | Frame p95 | Backend mean | Backend p95 | GPU interval |
|---|---:|---:|---:|---:|---:|
| A1 reference | 24.2156 | 33.6190 | 15.5876 | 24.8779 | 18.5903 |
| B1 batch | 19.9325 | 24.1666 | 5.0676 | 8.7309 | 19.4446 |
| B2 batch | 19.2023 | 23.0943 | 5.0618 | 8.5315 | 18.7554 |
| A2 reference | 20.7928 | 28.7110 | 11.1806 | 19.1039 | 19.4817 |
| A3 reference | censored: minimized HWND, exit 23 | — | — | — | — |
| B3 batch | не запущен после failure gate | — | — | — | — |

В первой паре backend wall уменьшился на 67.49%, frame на 17.69%; во второй — на 54.73% и 7.65%. Backend p95 уменьшился на 64.91% и 55.34%. Прогрев: backend mean A1/B1 15.6479/5.1024, A2/B2 11.4917/5.5787; warm frame p95 31.8488/22.3180 и 29.1212/27.2548. Регрессии этих конкретных показателей не наблюдалось, но третьей пары нет.

`process-monitor.ps1` сохраняет PID/name/cumulative CPU для всех доступных процессов раз в секунду. Анализ интегрирует CPU deltas на measured interval с пропорциональным распределением граничных samples; это приблизительная оценка, не ETW scheduler trace. В A1 несколько git-процессов занимали по ~0.74 ядра; в B1/B2 были git-процессы около одного ядра и другие фоновые приложения. A2 имел иной набор нагрузки. Отсутствие compiler hits не делает эти runs uncontended.

Есть существенный tradeoff: CPU-time **всего процесса** за измеряемый интервал вырос примерно с 95.73 до 105.59 CPU-s в первой паре и с 96.85 до 105.69 CPU-s во второй. Это около +10.3%/+9.1%, при меньшем critical-path wall. Увеличение параллелизма, spin и работа драйвера пока не разделены. Заявлять снижение общего CPU consumption или энергопотребления нельзя.

## Корректность и lifetime

Прототип находится только в `Build/PhysicsJobsPerformance/CandidateProduction.cpp` / `candidate.diff` (+41/-1 строка в одном `.cpp`). Он создаёт `ScheduledJob::Batch : enki::ITaskSet(count)` с `m_MinRange=1`. Enki дробит диапазоны для параллельного выполнения; whole batch не принудительно сериализуется. Каждый Jolt job получает AddRef до публикации task. Completion освобождает все refs и batch, затем уменьшает `_outstanding`; Drain ждёт окончания enki wrappers, даже если Jolt barrier выполнил jobs раньше. Singleton остаётся на исходном QueueJob. Новый JobSystem API и изменения enki не требуются.

Проверен контракт установленной версии enki: `TaskScheduler.cpp:628–665` сохраняет next dependency до callback и допускает удаление внутри OnDependenciesComplete. После callback текущий task не читается. `AddTaskSetToPipeInt` после завершающего TaskComplete обращается только к scheduler thread state; стартовый running count удерживает task во время первоначального SplitAndAddTask. Base Completion вызывается до освобождения объекта. Allocation/refcount/error semantics не заменялись подавлением исключений; общий JobSystem error path не изменён.

Проверки:

- `ProbeCheck`: nested inclusive/exclusive accounting, разделение потоков, выключенные scopes, stage mapping — PASS.
- `BatchCheck`: 1/4/32 threads, batches 1/2/31/128/513, 16 повторов каждого размера, early release handles, nested jobs и fan-in, exact-once counts, barrier/drain/destruction — PASS. Наблюдались 1/4/32 execution threads.
- Empty barrier/drain/destructor — PASS. Прямой private QueueJobs(count=0) не вызывался: публичный Jolt sRemoveDependencies требует непустой вход; отсутствие ready jobs не вызывает QueueJobs. Нулевой branch дополнительно проверен чтением.
- Full-pipe single-thread test: 256 queued jobs, batch выполняется inline до возврата QueueJobs, все refs и последующий Drain корректны — PASS.
- Существующий `Engine/Engine.Core/tests/JobSystemTests.cpp`: ranges, dependency chains/fan-in, synchronous work, error propagation/skipped dependents, reuse, 1/4 threads — PASS.
- Baseline repeat `state-probe-1/2`: 1321/1321 совпадений. Baseline с cycle probes также совпал с первым baseline.
- Candidate `state-batch-retry` против `state-cycles-1`: 1321/1321 совпадений FNV-1a-64 hash и byte count **полного** JPH SaveState, active/total bodies. Это hash oracle, а не сохранённое побайтовое сравнение всех сериализованных buffers.

Executable builds завершились; initial harness fixes касались MSVC getenv warning, синтаксиса теста и повторного vcvars/PATH в batch launcher. `build-window2.json` отражает неуспех launcher после успешной сборки executable; отдельный `batch-check-run.json` — PASS. Link warnings LNK4099 относятся к отсутствующему Jolt PDB и не скрывались.

## Сворачивание окна и исключённые прогоны

`state-batch-1` завершился с `Invalid perspective light grid inputs`. Первоначально отсутствие CSV ошибочно предполагалось признаком startup failure: CSV записывается только в конце, поэтому этот вывод не подтверждён.

Targeted `state-grid-1` сохранил реальные inputs и stack: на **616-м тике** render size 985×616, near≈0.1/far=10000 и settings валидны; `projection._11=inf`; собственное видимое окно `iconic=1`, client rect 0×0. Источник: WM_SIZE обновляет width=0 (height clamped to 1), GetAspectRatio становится 0; BenchmarkFrame продолжает Update/Render после minimize. Guard остановил рендер корректно. Ни guard, ни production renderer не исправлялись. Собственный уже завершивший shutdown процесс остановлен по exact PID/path, поскольку error MessageBox удерживал его живым; runner помечает run failed.

Последний state retry завершился успешно, 176 HWND samples покрывают измеряемый интервал без minimize. Window monitor на выходе столкнулся с гонкой QueryFullProcessImageName после завершения процесса; событие было после measured end. Узкая проверка exit code original process handle исправлена перед normal pairs, identity checking живого процесса сохранён.

В normal A1/B1/B2/A2 монитор работал без ошибок и покрывал весь measurement. A3 в `2026-11-12T12:37:12.901566Z` зафиксировал iconic=1/client0×0 и завершил **только собственный benchmark PID 70728** с кодом 23. B3 не запускался. Новых retries после оговорённого лимита не выполнялось. Окно не восстанавливалось в цикле, чужие окна не менялись.

## Артефакты и дальнейший gate

Все evidence — локально в ignored `Build/PhysicsJobsPerformance/`:

- `summary.json`, `final-summary.txt`, `cycles-summary.json`, `state-equivalence.json`, `candidate-state-equivalence.json`.
- На каждый run: runner JSON/fingerprints, launch JSON, result JSON, CPU CSV, GPU JSON, processes JSONL; deep runs дополнительно jobs CSV; monitored runs windows JSONL и monitor log.
- `state-grid-1-jobs.csv.invalid-grid.txt` содержит invalid matrices, scalar inputs, own HWND и stack addresses.
- `source-hashes.json`, `dependency-hashes.json`: External в accepted является junction, поэтому Jolt library/headers и enki headers хешировались отдельно; drift не обнаружен. Failed state-batch-1 имел 110 совпадающих before/after fingerprints. Нормализованные deep link commands различаются только Adapter object и именами выходов.
- `prepare.py`, `make_candidate.py`, `prepare_checks.py`, `prepare_grid_diagnostic.py`, `run.py`, `analyze.py`, probes/tests/response files. Первая версия harness сохранена в `v1/`. Link `/LTCGOUT` перенаправлен в собственный Build, accepted artifacts не перезаписывались.

Ключевые SHA256:

| Артефакт | SHA256 |
|---|---|
| Exact reference executable | `bd6210f2b0f77bfdb32f80ab149a635bee49a37f3d01022d72fc912be5f11fd4` |
| Normal batch executable | `c8f5661ae52f640e45a81e3909730b89ee540fc1e9675a1f52122afe02f7d17b` |
| Deep baseline cycles executable | `4dffd405006f6126c716343a087e4d95d300932382a47b599ccacc2a9440d854` |
| Deep candidate + invalid-grid diagnostic | `041d0037d30fccd11161a19b1d90526c23f05884719867686d9938e36caca508` |
| Candidate diff | `88156e6d0479bd3063f741a99ccc0f5ebef1a4f4c7cab27f978d88eac25f8d3c` |
| Candidate state-equivalence JSON | `090b4710ead7fa03c9123119b5b31813f4a37229a52c3e07e8f6cb98216fe493` |

Offline повтор анализа: `python Build/PhysicsJobsPerformance/analyze.py fixed-reference-1 fixed-probe-1 state-probe-1 state-probe-2 state-cycles-1 state-batch-retry normal-a1 normal-b1 normal-b2 normal-a2`.

Для production acceptance остаются: законченная серия минимум трёх пар с контролируемой внешней нагрузкой и несвёрнутым окном; overhead check; отдельный ordinary real-time результат с ticks/debt и оценка роста total CPU-time. Текущая production интеграция с более поздним HEAD не собиралась и не проверялась. Пока принимается только этот диагностический отчёт; promising batch patch сохранён отдельно для следующего разрешённого эксперимента. Все мои CPU/GPU/build процессы завершены, окно RELEASE; локальный коммит отчёта выполняет общий ревьюер.

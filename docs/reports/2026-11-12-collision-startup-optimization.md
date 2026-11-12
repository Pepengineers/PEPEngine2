# Bistro: холодная подготовка коллизий

Статус: диагностическая итерация завершена, production-кандидат не принят.
Кандидат сохраняет точный результат и сокращает измеренную CPU-работу и peak memory,
но обе диагностические пары показали ухудшение wall time. Вторая пара локализовала
разницу внутри `VHACD::Compute` при существенно разной внешней Git-нагрузке;
причина ожиданий/descheduling внутри этой стадии ещё не установлена.

## Область и исходные данные

- Принятый исходный snapshot: `7acaf95089541909925d8e305fa3b0ba55ff5cc8`.
  Чужой MGPU diff и shared build outputs не используются и не изменяются.
- CPU: Ryzen 9 5950X, 16 физических ядер / 32 logical processors.
- Исходный обычный exe: `Build/PerformanceAnalysis/runtime/BistroAcceptedRelease.exe`,
  SHA-256 `4263cb31ddd7ddf4106e76329e44827fe50e771b0769cfe20d3dcd1ab7e5174d`.
  Он не запускался в первом collision-окне.
- `Build/PerformanceAnalysis/accepted` — чужая frozen source copy и build evidence;
  только чтение. Её `.obj/.lib` собраны с `PEPEnableProfiling=true`; их нельзя
  выдавать за обычную Release-сборку.
- В исходном коротком окне `cold-early-stacks.tsv` main ждёт `BakeColliders`,
  31 worker выполняет VHACD. `cold-stack-summary.json` относит все наблюдаемые
  CPU increments этого окна к collision workers. Это атрибуция окна, а не
  доказательство критического пути всей холодной загрузки.
- Старые cold/первый warm пересеклись с внешними builds. Их elapsed не принят
  как чистый baseline и не используется для оценки выигрыша.

## Проверенный кандидат

В `External/External.Libraries/vhacd/VHACD.h`, `VoxelHull::BuildRaycastMesh`,
в scratch-копии заменено одно условие:

```cpp
// Only best-plane splitting raycasts against the voxelized surface mesh.
if ( m_params.m_findBestPlane && !m_indices.empty() )
```

Исходный guard проверяет только `!m_indices.empty()`. Исходный header SHA-256:
`c2b750a41bc6044457c79dc51d196b7bbe5e24bfaec72d409a065434d9651ff2`.
Оригинал в shared checkout пока не изменён. Scratch-копии:
`Build/CollisionStartup/VHACD.baseline.h` и `VHACD.candidate.h`.

Все чтения именно `VoxelHull::m_AABBTree` идут через `Raycast` → `FindConcavity`
→ X/Y/Z wrappers → `ComputeSplitPlane`. Все три вызова gated
`m_params.m_findBestPlane`. Root constructor копирует параметры, child constructor
копирует их у parent до построения дерева; внутренний async использует те же
constructors. Значение по умолчанию и значение Bistro — `false`.

Отдельное `VHACDImpl::m_AABBTree` нужно для voxelization и shrinkwrap и не меняется.
`BuildVoxelMesh`, QuickHull, параметры качества, исходная геометрия, cache key,
валидация cache и правила box proxies не меняются. Версия зависимости не меняется.
Переиспользование похожих/повёрнутых world-space meshes не применяется.

## Корректность первого окна

`Build/CollisionStartup/build.cmd` собирает два standalone oracle-exe и два
Bistro kernel-exe. Все выходы и cache принадлежат `Build/CollisionStartup`.
`build-harness.json` — успешная сборка; команды компиляции и линковки в соседних
`.rsp`. Общий `Build/AssetCache/Collision` не удалялся и не перемещался.

1. `VHACDRaycastTests.cpp`: concave L из двух примыкающих boxes, три ориентации
   с разными самыми длинными осями, `findBestPlane=false/true` ×
   `m_asyncACD=false/true`: 12 случаев. Каждый обязан выдать больше одного hull,
   поэтому recursive split действительно выполняется; true-проверки доходят до
   concavity path. Разрешение 100000, recursion depth 4, hull budget 8,
   максимум 32 vertices, volume error 0.01.
2. Baseline пишет `vhacd-oracle.bin`; candidate сравнивает полное содержимое:
   count, порядок hulls/vertices/triangles, координаты double, индексы, volume,
   mesh id, center и bounds. Padding C++ structs не сериализуется.
   **PASS: 12/12, 27816 байт точного output.**
   Raw: `oracle-baseline.*`, `oracle-candidate.*`.
3. Реальные Bistro inputs загружены production `LoadMeshCache` из
   `accepted/Build/AssetCache/2792230198490357785.mesh-1-6-0-4-8388648.pepmesh`.
   Frozen `PartitionBistroProps` дал 947 props и 42 non-tableware inputs.
   Использованы unchanged `Gather`/`BuildDynamicCollision`, error 1 cm и исходная
   формула hull budget 4..12; 31 worker. Оба прогона создали ровно 42 cache файла.
   **Все filenames и SHA-256 совпали между A/B и исходными 42 accepted cache.**
   Raw: `kernel-A1-cache-hashes.json`, `kernel-B1-cache-hashes.json`.
   Независимый пересчёт самих файлов: `Build/CollisionStartup/cache-comparison.json`.
   В нём все 42 имени/SHA для каталогов
   `Build/CollisionStartup/{kernel-A1,kernel-B1,diagnostic-A2,diagnostic-B2}/Build/AssetCache/Collision`
   и `Build/PerformanceAnalysis/accepted/Build/AssetCache/Collision`.
   Все пять наборов равны; четыре исходных JSON exports повторно проверены:
   каждый содержит 42 записи / 4035 байт и соответствует своему каталогу.
4. Существующий frozen `CollisionGeometryTests.cpp` запущен с candidate и
   собственным cache root. **PASS:** box/sphere/capsule/cylinder fitting, baked
   pivots, bottle shoulder/hollow shell, static QEM, отверстия/границы, warm cache,
   invalidation, corrupt-cache rebuild, concave chair и gap regressions.
   Raw: `build-existing.*`, `existing-tests.*`. Код теста не менялся.

Эти проверки доказывают совпадение проверенных результатов. Полного App/frame A/B
и общей проверки всех возможных VHACD inputs они не заменяют.

## Первая диагностическая kernel-пара: не принята как ускорение

Kernel-exe компилируют Collision TU с `/O2 /Ob2 /GL /MD /fp:precise`, без
`PROFILE_BUILD`, но линкуются с frozen support libraries из profiling build.
Поэтому это отдельный диагностический harness, а не ordinary App throughput.
Импорт input cache выполняется до измеряемого scope; scope включает создание
31 threads, все 42 `BuildDynamicCollision`, запись cache и join.

| Показатель | Baseline A1 | Candidate B1 |
|---|---:|---:|
| Collision wall, s | 42.9752839 | 52.7423043 |
| Полный runner elapsed, s | 43.53 | 53.25 |
| Observed worker-process CPU increment, s | 206.438 | 74.422 |
| Observed Unreal Editor CPU increment, s | 6.516 | 9.672 |
| Новые cache files | 42 | 42 |

Candidate wall хуже примерно на 22.7%. Меньший sampled CPU increment **не считается
выигрышем wall time**. Это одна пара, не требуемые три чистых парных повтора.

CPU increments получены из первого и последнего доступного process sample, а не
из process start/exit. Первый sample уже содержал 52.188 CPU s для A и 27.016 CPU s
для B, поэтому это неполные CPU totals. В обоих интервалах обнаружен пользовательский
`UnrealEditor` PID 36404. Он не остановлен. Его активность сама по себе не доказывает
причину замедления; GPU contention отдельно не измерена. Compiler hits не обнаружены.
Raw process timeline: `kernel-A1-processes.jsonl`, `kernel-B1-processes.jsonl`.
Пара сохраняется полностью, включая плохой wall результат. Первый monitor не
записывал Git-процессы; их вклад в A1/B1 восстановить по этому timeline нельзя.

## Вторая пара: per-job attribution, порядок B2 → A2

Во втором разрешённом окне собраны отдельные diagnostic exe. Изменены только
scratch-копии: замеры wall/`GetThreadTimes` по стадиям и каждому job, вложенный
счётчик `BuildRaycastMesh`, `GetProcessTimes` и `GetProcessMemoryInfo` до/после
collision и перед выходом. Production-код не инструментировался. Здесь,
в отличие от первого окна, monitor записывает **все** процессы.

Raw: `Build/CollisionStartup/diagnostic-A2.*`, `diagnostic-B2.*`, соответствующие
`*-processes.jsonl`, `*-cache-hashes.json`, `*-engine.log`;
сводка — `diagnostic-summary-diagnostic-A2-diagnostic-B2.json`.
Подготовка и проверяемый анализ — `prepare_diagnostic.py`, `analyze_diagnostic.py`.
`build-diagnostic.json` и `build-diagnostic-processes.jsonl` сохраняют сборку.

| Показатель | Baseline A2 | Candidate B2 |
|---|---:|---:|
| Collision wall, s | 49.4702881 | 61.3683155 |
| Collision process CPU, s, полная граница scope | 278.078 | 114.953 |
| Process CPU перед выходом, s | 278.343750 | 115.296875 |
| BuildRaycastMesh calls | 39724 | 39724 |
| BuildRaycastMesh summed thread CPU, s | 172.343750 | 0.000000 |
| BuildRaycastMesh summed wall, s | 263.844883 | 0.051344 |
| Peak working set, bytes | 3337617408 | 1006800896 |
| Peak process commit, bytes | 4526514176 | 1265094656 |
| Page fault delta в collision scope | 4168389 | 1041172 |

Raycast CPU candidate округлён к разрешению `GetThreadTimes`, это не утверждение
о буквально нулевой стоимости guard/timer. Суммы времени параллельных вызовов
не складываются с полным wall. Page faults включают soft faults и не доказывают
дисковый paging. Allocation counts не измерялись.

Во второй паре все 42 cache снова побитно совпали с A1/B1/accepted. Guard выполняет
ровно предполагаемую работу: исключает voxel raycast trees при false; их отсутствие
объясняет меньшую CPU-работу и peak memory в этой серии. Оно **не доказало ускорение
холодной загрузки**: wall candidate хуже примерно на 24.0%.

Последним в обоих случаях завершился `Paris_Table_02A`:

| Показатель job | A2 | B2 |
|---|---:|---:|
| Старт относительно начала collision, s | 37.278147 | 50.423187 |
| Завершение, s | 49.470089 | 61.368254 |
| VHACD Compute wall, s | 12.163301 | 10.925977 |
| VHACD Compute thread CPU, s | 6.875000 | 1.937500 |
| Вложенный raycast thread CPU, s | 4.234375 | 0.000000 |

У остальных последних jobs та же картина. Gather, primitive fit, near-convex,
simplification, cache IO и DynamicShape малы; сумма wall всех `Save` составляет
0.089 s у A2 и 0.078 s у B2. Задержка находится **внутри Compute**, а не в
предположенном долгом NearConvex или записи cache. B2 позднее освобождает workers
первой группы: последний job стартует на 13.145 s позже, хотя его собственная
вычислительная работа меньше. Для него разница Compute wall минус thread CPU —
около 5.29 s у A2 и 8.99 s у B2. Это время неисполнения потока на CPU; данные
ещё не различают ожидание синхронизации/allocator, page faults и descheduling.

Внешняя нагрузка в паре существенно различается. Только три наиболее активных
наблюдаемых Git-процесса у B2 дали дельты 151.016, 46.156 и 26.000 CPU s
(223.172 s суммарно); у A2 — 42.297, 23.109 и 21.484 (86.890 s).
Это first-to-last deltas наблюдаемых PID, не полный расход всех Git-процессов
за интервал. Unreal Editor дал 10.047 s у B2 и 7.391 s у A2. Compiler hits не
обнаружены. Чужие приложения и фоновые scans не останавливались.

Такая пара пригодна для локализации стадий и проверки output, но не даёт чистого
baseline/candidate speedup. Git-нагрузка — наблюдаемый confounder, а не доказанная
единственная причина всей разницы. Прямых worker stacks/wait attribution в этом
втором окне не снимали. Дополнительные дорогие full-App повторы при этом состоянии
не запускались.

## Что остаётся

- Конкретный diagnostic gap: снять короткие worker stacks/wait attribution
  **внутри Compute** при сопоставимой внешней нагрузке, отделить allocator/другие
  ожидания от descheduling. Проверять прежде всего workers первой группы,
  удерживающие запуск последних jobs; просто повторять полный App пока не нужно.
- Только после объяснения результата: clean ordinary App baseline/candidate,
  не менее трёх cold пар с отдельными owned caches и warm/frame regression,
  одинаковые качество/сцена/seed/числа объектов, hashes и process timeline.
  Подготовлены, но пока не запускались `prepare_ordinary.py` и `run_app.py`.
- Если candidate будет принят, обновить vendored README (сейчас там `Unmodified`),
  сохранить focused regression test и передать exact paths/hashes ревьюеру.

Цель ≥30% сокращения cold collision wall time **не достигнута**. Трёх чистых cold
пар и warm/frame regression нет; full-App ускорение и отсутствие frame-регрессии
не заявляются. Все GPU/build окна освобождены для остальных задач. На review
передаётся только этот отчёт, shared VHACD остаётся исходным. Коммит самостоятельно
не создан: новые scoped commits по условиям задачи выполняет общий ревьюер.

Freeze manifest: `Build/CollisionStartup/ready-evidence-sha256-v2.json`.
Первый `ready-evidence-sha256.json` сохранён как evidence предыдущей редакции отчёта;
исходные logs, binaries и timing/cache exports между редакциями не переписывались.

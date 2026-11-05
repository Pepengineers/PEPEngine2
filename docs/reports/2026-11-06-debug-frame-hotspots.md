# Остаточная стоимость Debug Bistro

Исходная версия: `fef6d02`. Замеры 6 ноября 2026, Ryzen 7 5800HS,
RTX 3060 Laptop 6 GiB, NVIDIA 617.42, Debug x64. Изучается обычный App
со стартовой камерой у входа, окном 1280×800, `Render.NumFrames=3`,
`App.Bounds.Show=false`, particle scale 1 / budget 250000. Качество эффектов,
состав сцены, assertions, Debug CRT и физические настройки сохранены.

## Где проходило время

Свежий подробный профиль исходной версии, 110 кадров после исключения 0–10:

| CPU wall time | Среднее, мс |
|---|---:|
| Render целиком | 138,70 |
| Подготовка pipeline, вложена в Render | 118,47 |
| Snapshot сцены, вложен в подготовку | 33,88 |
| Ожидание shadow visibility, вложено в подготовку | 70,93 |
| Lighting preparation, включая выполнение доступных jobs | 7,81 |
| Запись графа | 14,47 |
| Update целиком | 28,14 |
| Физика, вложена в Update | 20,92 |
| BeginFrame, вложен в Update | 5,21 |

Это вложенные интервалы, их нельзя все складывать. В частности, главный поток
может выполнять shadow jobs при ожидании light-grid jobs; отдельные 70,93 мс
ожидания не описывают полную стоимость shadow visibility.

GPU timestamp interval в этом прогоне — 47,17 мс. Отдельный Nsight GPU Trace
содержит три кадра (515,82 мс): GR Engine Active 24,87%, SM throughput 5,90%,
VRAM throughput 7,59%, PCIe throughput 18,28%. Из top-level GPU markers
локальные тени занимали 16,86 мс/кадр, солнечные каскады — 8,25 мс,
GBuffer — 2,37 мс, SSR — 2,21 мс. Основное ограничение этого сценария — CPU.
Оставшиеся GPU-затраты на тени также значительны.

Захват сделан native Nsight Graphics MCP, Nsight Graphics 2026.4.
Его WRPV v11 не поддерживается установленным `nsight_trace_analysis`, поэтому
использован native `gpu_trace.open_trace` и запросы markers/metrics.
Предупреждение захвата сообщает об отсутствующих данных трёх shader metrics;
общие throughput counters и marker intervals доступны. Это один короткий
захват, не статистика всего уровня.

## Изменения

- Плоскости и абсолютные нормали shadow frustum подготавливаются один раз
  на грань света. Проверка AABB использует те же clip-space inequalities,
  без повторного построения плоскостей и цепочки незаинлайненных Debug
  SIMD helpers для каждого объекта.
- Проверка дальности света использует расстояние от его центра до AABB
  в скалярной форме. Это та же sphere/box проверка, включая касание,
  без вызовов общего SIMD collision path для каждой пары light/caster.
- Shadow visibility распределяется заданиями по одному свету: количество
  попадающих в каждый свет объектов сильно различается.
- Snapshot один раз получает указатели instance/transform CPU mirrors.
  Его mesh primitive records хранятся по существующим GPU instance indices
  в vector вместо hash table. Генерации primitive handles, обновление epoch,
  удаление пропавших объектов и повторное использование handles сохранены.
- Циклы Bistro geometry получают вместимость views один раз. Раньше условие
  каждого шага заново обходило lights для вычисления той же вместимости.

Новых зависимостей и настроек нет.

## Измерения вариантов

Первый обычный App после подготовки frustums, изменения размера jobs и hoisting
frame pointers дал 147,757 мс. После скалярной range check диагностический
App дал 112,519 мс. Это одиночные промежуточные прогоны, не независимые оценки
вклада каждого изменения и не финальная статистика.

Во втором профиле ожидание shadow visibility сократилось до 15,52 мс,
pipeline preparation — до 57,31 мс. Snapshot оставался 33,58 мс:
17,25 мс извлечение/публикация, 8,67 мс агрегация примитивов,
2,39 мс bounds для каскадов, около 5,26 мс прочая работа, включая retirement.
Это основание для последнего варианта с прямой индексацией records.

Итоговый diagnostic executable дал 93,225 мс. В его прогретой части:

| CPU wall time | До, мс | Итог, мс |
|---|---:|---:|
| Render | 138,70 | 61,21 |
| Pipeline preparation | 118,47 | 41,26 |
| Scene snapshot | 33,88 | 16,99 |
| Shadow visibility wait | 70,93 | 19,91 |
| Lighting preparation | 7,81 | 2,08 |
| Graph recording | 14,47 | 13,89 |
| Physics | 20,92 | 21,85 |
| BeginFrame | 5,21 | 5,78 |

После прямой индексации извлечение/публикация snapshot сократилось с 17,25
до 5,09 мс; весь snapshot — с 33,58 до 16,99 мс. Это больше предварительного
ожидания 5–12 мс. Общая разница диагностических вариантов включает изменение
стоимости записи shadows и scheduling; её нельзя целиком приписать vector.
Первые shadow changes ожидались в пределах 20–50 мс; измеренные интервалы
visibility и pipeline подтверждают направление, но не независимый вклад
каждой из сгруппированных правок.

Остаточные CPU-затраты: физика ~22 мс, shadow visibility ~20 мс,
snapshot ~17 мс (из них агрегация примитивов ~9,3 мс), запись графа ~14 мс
(из них локальные тени ~9,5 мс). GPU timestamp interval — ~30,1 мс;
из-за отличающегося времени симуляции это не оценка ускорения GPU-алгоритмов.
Physics backlog этим профилем не измеряется.

Финальное сравнение обычных executable: три чередующиеся пары, по 120
измеряемых кадров на запуск, без наших сборок/тестов во время замеров.
Исходный executable сохранён до пересборки как `Baseline.exe` в том же
runtime directory; его SHA256 проверен против исходного App.exe
`bee029d31559e3011aedfa129e47b5e9adc85407606308b7319542a722309717`.
INI общий, его hash проверяется перед каждым запуском.

| Пара | Исходный App, мс | Итоговый App, мс | Снижение |
|---|---:|---:|---:|
| 1 | 191,439 | 102,376 | 46,52% |
| 2 (обратный порядок) | 193,416 | 106,592 | 44,89% |
| 3 | 193,746 | 104,657 | 45,98% |
| Медиана трёх запусков | **193,416** | **104,657** | **45,89%** |

Разница медиан — **88,759 мс**, ускорение **1,848×**,
производная частота — **5,17 → 9,56 FPS**. Все шесть запусков завершились
с exit 0. Разброс средних по запускам: baseline 191,439–193,746 мс,
итог 102,376–106,592 мс; sample CV соответственно 0,65% и 2,02%.
Наблюдаемый выигрыш в каждой из трёх пар — 44,89–46,52%; это эмпирический
диапазон, не доверительный интервал. Каждое состояние содержит 360 измеренных
кадров. Разница значительно превышает наблюдаемый разброс короткой серии.

Ранее сообщённые 173,789 мс и первый текущий прогон 172,684 мс не смешиваются
с этой серией. В финальном окружении тот же исходный executable действительно
работал медленнее; поэтому для оценки выигрыша используется свежий baseline.
Выбор изменений и итоговая оценка используют один стартовый сценарий,
отдельного holdout нет. Это не замер прохождения всей сцены.

Сохранены три варианта изменений; итог подтверждён относительно исходного
executable, без попытки складывать отдельные проценты ускорения.
После третьего варианта серия завершена. Новые зависимости не использовались;
публикация и push не выполнялись. Машиночитаемые данные:
`paired-results.json`, `confirmation-summary.json`, `confirmation-hashes.json`.

## Проверки

- App собран в Debug и Release; App.Editor — в Debug.
- LightingGpuTests прошёл в Debug и Release: подготовленные point/spot frustums
  и scalar range check сверены с SIMD/DirectXCollision на 117760 seeded
  сочетаниях bounds/граней. Сохранены проверки cube seams, касания дальности,
  six-face coverage и совпадения jobs на 1/4 потоках. GPU readbacks проверяют
  shadow caching/invalidation, deferred и forward illumination; Debug layer
  не сообщает ошибок.
- RenderPipelineTests и ObjectDataJobsTests прошли на итоговом коде в Debug
  и Release: primitive identity/generation/retirement, sparse instance indices,
  недопустимый UINT_MAX, GPU uploads во всех frame slots, изменение transform,
  material/visibility/classification, reimport, удаление и замена.
- BistroRenderGraphTests прошёл в Debug: настоящая grass geometry, одинаковый
  HDR pooled/dedicated и direct/async, обе позиции AO, odd resize и Native/XeSS.
- `git diff --check` прошёл; cumulative diff просмотрен локально.

Сборки сохраняют C4267, отсутствующие vendor PDB (LNK4099); Release link
также сообщил LNK4020 для type records External.Libraries.pdb. Это ограничивает
доступность некоторых vendor symbols. Ошибка C1902 первой sandbox-сборки
устранена повторной сборкой в окружении Visual Studio вне sandbox.

## Воспроизведение и артефакты

```powershell
Build\bin\x64\Debug\App\App.exe `
  -Loading.BenchmarkOutput=D:\Project\PEPEngine2\Build\debug-frame.json `
  -Loading.BenchmarkFrames=121
```

`Build/DebugPerf-20261106/` содержит diagnostic copies, response files,
`prepare.py`, `build.py`, `summarize.py`, CSV/JSON profiles,
`baseline.ngfx-gputrace`, `gpu-overview.json`, build/test logs,
`spec.yaml` и `experiment-log.yaml`. Build игнорируется Git.

Diagnostic scopes добавлены только в копии translation units; обычный App
не содержит этого instrumentation. Первый общий baseline 172,684 мс
частично пересекался со сборкой diagnostic executable; он используется
только как воспроизведение симптома и исключается из финального сравнения.

Частоты/температура не фиксировались. Физика и seeded waves идут по реальному
времени: более быстрые 120 кадров охватывают меньше секунд симуляции.
Поэтому GPU interval и время физики между вариантами не доказывают ускорение
их алгоритмов. Новый профиль и парные прогоны ограничены стартовым ракурсом;
длительный soak, scale 16 и другое оборудование не входят в проверку.
